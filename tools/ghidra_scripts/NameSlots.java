// Names virtual slots across a whole class family from slot_names.tsv (base_class, slot, name, evidence):
//  - the implementation in every family vtable (base and overrides) is renamed, if it still has a structural name
//    (vfNN, or the address-suffixed vfNN_xxxxxx); each keeps its own class namespace
//  - the matching field of every family vtable struct is renamed, so calls read this->vftable->Update(...)
// Also renames struct fields listed in field_names.tsv (base_class, offset, name) in every family class struct.
// Args: <slot_names.tsv> <classes.tsv> [field_names.tsv]
// @category Digimon
import java.io.BufferedReader;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeComponent;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.Structure;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;

public class NameSlots extends GhidraScript {
	private final Map<String, Long> vtableOf = new HashMap<>();      // current class name -> vtable address
	private final Map<String, List<String>> children = new HashMap<>();
	private final Map<String, String> currentName = new HashMap<>(); // original Class_ id -> current name

	private String nameAt(long vt) {
		for (Symbol s : currentProgram.getSymbolTable().getSymbols(toAddr(vt))) {
			if (s.getName().endsWith("_vftable")) {
				return s.getName().substring(0, s.getName().length() - "_vftable".length());
			}
		}
		return null;
	}

	private List<String> family(String base) {
		List<String> out = new ArrayList<>();
		List<String> stack = new ArrayList<>(List.of(base));
		while (!stack.isEmpty()) {
			String c = stack.remove(stack.size() - 1);
			out.add(c);
			stack.addAll(children.getOrDefault(c, List.of()));
		}
		return out;
	}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		DataTypeManager dtm = currentProgram.getDataTypeManager();
		List<String[]> rows = new ArrayList<>();
		try (BufferedReader in = new BufferedReader(new FileReader(args[1]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				String[] c = line.split("\t", -1);
				rows.add(c);
				String cur = nameAt(Long.parseLong(c[1], 16));
				if (cur != null) {
					currentName.put(c[0], cur);
					vtableOf.put(cur, Long.parseLong(c[1], 16));
				}
			}
		}
		for (String[] c : rows) {
			String child = currentName.get(c[0]);
			if (child == null || c[4].isEmpty()) {
				continue;
			}
			String parent = currentName.get(c[4].split(",")[0]);
			if (parent == null) {
				// classes.tsv stores parents by their names at recovery time; map them through class_names if needed
				parent = c[4].split(",")[0];
			}
			children.computeIfAbsent(parent, k -> new ArrayList<>()).add(child);
		}

		int funcs = 0, fields = 0;
		try (BufferedReader in = new BufferedReader(new FileReader(args[0]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				String[] c = line.split("\t");
				String base = c[0];
				int slot = Integer.parseInt(c[1]);
				String name = c[2];
				for (String cls : family(base)) {
					Long vt = vtableOf.get(cls);
					if (vt == null) {
						continue;
					}
					Function f = getFunctionAt(toAddr(getInt(toAddr(vt + 4L * slot)) & 0xFFFFFFFFL));
					if (f != null && f.getName().matches("vf\\d\\d(_[0-9a-f]+)?")) {
						f.setName(name, SourceType.USER_DEFINED);
						funcs++;
					}
					DataType vtt = dtm.getDataType("/DRA2/vtables/" + cls + "_vftable");
					if (vtt instanceof Structure) {
						DataTypeComponent comp = ((Structure) vtt).getComponentAt(slot * 4);
						if (comp != null && comp.getFieldName() != null && comp.getFieldName().startsWith("slot")) {
							comp.setFieldName(name);
							fields++;
						}
					}
				}
			}
		}
		int structFields = 0;
		if (args.length > 2) {
			try (BufferedReader in = new BufferedReader(new FileReader(args[2]))) {
				in.readLine();
				for (String line; (line = in.readLine()) != null;) {
					String[] c = line.split("\t");
					int off = Integer.parseInt(c[1].replace("0x", ""), 16);
					for (String cls : family(c[0])) {
						for (java.util.Iterator<Structure> it = dtm.getAllStructures(); it.hasNext();) {
							Structure s = it.next();
							if (!s.getName().equals(cls) || !s.getCategoryPath().isRoot()) {
								continue;
							}
							DataTypeComponent comp = s.getComponentAt(off);
							if (comp != null && comp.getOffset() == off) {
								comp.setFieldName(c[2]);
								structFields++;
							}
						}
					}
				}
			}
		}
		println(String.format("renamed %d slot implementations, %d vtable fields, %d class fields", funcs, fields,
			structFields));
	}
}
