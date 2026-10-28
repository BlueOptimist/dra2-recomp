// Extends recovered vtables after DefineMissingFunctions.java: a vtable whose run of function pointers is now
// longer than its vftable struct gets the extra slots appended (slotNN_xxxxxx, void*), and every slot function
// still in the global namespace with a default FUN_ name moves into the class as vfNN. Existing slot names,
// types and methods are left alone. Writes classes.tsv with the new slot and owned-method counts.
// Args: <classes.tsv in> <classes.tsv out>
// @category Digimon
import java.io.BufferedReader;
import java.io.FileReader;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.Structure;
import ghidra.program.model.data.VoidDataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;

public class ExtendVtables extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		List<String[]> rows = new ArrayList<>();
		String header;
		try (BufferedReader r = new BufferedReader(new FileReader(args[0]))) {
			header = r.readLine();
			for (String line; (line = r.readLine()) != null;) {
				rows.add(line.split("\t", -1));
			}
		}
		Set<Long> vtables = new HashSet<>();
		for (String[] c : rows) {
			vtables.add(Long.parseLong(c[1], 16));
		}
		Memory mem = currentProgram.getMemory();
		DataTypeManager dtm = currentProgram.getDataTypeManager();
		int extended = 0, moved = 0;
		for (String[] c : rows) {
			long vt = Long.parseLong(c[1], 16);
			int known = Integer.parseInt(c[2]);
			List<Function> slots = new ArrayList<>();
			for (long a = vt; slots.isEmpty() || !vtables.contains(a); a += 4) {
				Function f = getFunctionAt(toAddr(mem.getInt(toAddr(a)) & 0xffffffffL));
				if (f == null) {
					break;
				}
				slots.add(f);
			}
			if (slots.size() <= known) {
				continue;
			}
			// the vtable label carries the class's current name (<Name>_vftable)
			Symbol label = getSymbolAt(toAddr(vt));
			String name = label != null && label.getName().endsWith("_vftable")
				? label.getName().substring(0, label.getName().length() - "_vftable".length()) : c[0];
			DataType dt = dtm.getDataType("/DRA2/vtables/" + name + "_vftable");
			if (dt == null) {
				dt = dtm.getDataType("/DRA2/vtables/" + c[0] + "_vftable");
			}
			Namespace ns = currentProgram.getSymbolTable().getNamespace(name, currentProgram.getGlobalNamespace());
			if (!(dt instanceof Structure) || ns == null) {
				printerr("no vftable struct or namespace for " + name);
				continue;
			}
			Structure vs = (Structure) dt;
			for (int i = vs.getLength() / 4; i < slots.size(); i++) {
				vs.add(new PointerDataType(VoidDataType.dataType), 4,
					String.format("slot%02d_%06x", i, slots.get(i).getEntryPoint().getOffset()), null);
			}
			Set<Function> owned = new HashSet<>();
			for (int i = 0; i < slots.size(); i++) {
				Function f = slots.get(i);
				if (f.getParentNamespace().isGlobal() && f.getName().startsWith("FUN_")) {
					f.setParentNamespace(ns);
					f.setName(String.format("vf%02d", i), SourceType.ANALYSIS);
					moved++;
				}
				if (f.getParentNamespace().equals(ns)) {
					owned.add(f);
				}
			}
			println(String.format("%s (%s): %d -> %d slots", name, c[0], known, slots.size()));
			c[2] = Integer.toString(slots.size());
			c[6] = Integer.toString(Math.max(owned.size(), Integer.parseInt(c[6])));
			extended++;
		}
		try (PrintWriter out = new PrintWriter(args[1])) {
			out.println(header);
			for (String[] c : rows) {
				out.println(String.join("\t", c));
			}
		}
		println(String.format("extended %d vtables, moved %d methods into their classes", extended, moved));
	}
}
