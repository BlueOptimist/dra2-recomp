// Post-processing for RecoverClasses.java:
//  - renames classes from class_names.tsv (class namespace, vtable struct and label)
//  - rebuilds every class struct from classes.tsv under the class's current name: smallest observed object size
//    (larger sizes come from subclasses whose constructors were inlined) and a typed vftable field at offset 0
//  - class functions get __thiscall, so Ghidra supplies a typed 'this'
// Args: <class_names.tsv> <classes.tsv>
// @category Digimon
import java.io.BufferedReader;
import java.io.FileReader;
import java.util.HashMap;
import java.util.Map;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.Structure;
import ghidra.program.model.listing.VariableUtilities;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.GhidraClass;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;

public class ClassFixups extends GhidraScript {
	@Override
	protected void run() throws Exception {
		DataTypeManager dtm = currentProgram.getDataTypeManager();
		// A class is identified by its vtable address (classes.tsv), so renaming works however often a class was
		// renamed before: its current name is the label "<name>_vftable" at that address.
		Map<String, Long> vtableOf = new HashMap<>();
		try (BufferedReader in = new BufferedReader(new FileReader(getScriptArgs()[1]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				String[] c = line.split("\t");
				vtableOf.put(c[0], Long.parseLong(c[1], 16));
			}
		}
		int renamed = 0;
		try (BufferedReader in = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				String[] c = line.split("\t");
				if (c.length < 2 || !vtableOf.containsKey(c[0])) {
					continue;
				}
				String current = null;
				for (Symbol s : currentProgram.getSymbolTable().getSymbols(toAddr(vtableOf.get(c[0])))) {
					if (s.getName().endsWith("_vftable")) {
						current = s.getName().substring(0, s.getName().length() - "_vftable".length());
					}
				}
				if (current == null || current.equals(c[1])) {
					continue;
				}
				Namespace ns = currentProgram.getSymbolTable().getNamespace(current, currentProgram.getGlobalNamespace());
				if (!(ns instanceof GhidraClass)) {
					continue;
				}
				ns.getSymbol().setName(c[1], SourceType.USER_DEFINED);
				DataType vt = dtm.getDataType("/DRA2/vtables/" + current + "_vftable");
				if (vt != null) {
					vt.setName(c[1] + "_vftable");
				}
				for (Symbol s : currentProgram.getSymbolTable().getSymbols(current + "_vftable")) {
					s.setName(c[1] + "_vftable", SourceType.USER_DEFINED);
				}
				renamed++;
			}
		}
		// rebuild class structs
		Map<String, String> finalName = new HashMap<>();
		try (BufferedReader in = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				String[] c = line.split("\t");
				if (c.length >= 2) {
					finalName.put(c[0], c[1]);
				}
			}
		}
		int structs = 0;
		try (BufferedReader in = new BufferedReader(new FileReader(getScriptArgs()[1]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				String[] c = line.split("\t");
				String original = c[0];
				String name = finalName.getOrDefault(original, original);
				Namespace byName = currentProgram.getSymbolTable().getNamespace(name, currentProgram.getGlobalNamespace());
				if (!(byName instanceof GhidraClass)) {
					continue;
				}
				// the class struct keeps whatever name it had when the class is renamed; find it by its vftable
				// comment ("vtable at <addr>") and rename it to match the class
				{
					DataType old = null, current = null;
					String tag = "vtable at " + c[1];
					for (java.util.Iterator<Structure> it = dtm.getAllStructures(); it.hasNext();) {
						Structure s = it.next();
						if (!s.getCategoryPath().isRoot()) {
							continue;
						}
						if (s.getName().equals(name)) {
							current = s;
						}
						else if (s.getNumComponents() > 0 && s.getComponent(0) != null &&
							tag.equals(s.getComponent(0).getComment())) {
							old = s;
						}
					}
					if (old != null && current == null) {
						old.setName(name);
					}
				}
				Namespace ns = currentProgram.getSymbolTable().getNamespace(name, currentProgram.getGlobalNamespace());
				if (!(ns instanceof GhidraClass)) {
					continue;
				}
				int size = 4;
				String sizes = c[3].replaceAll("[\\[\\] ]", "");
				if (!sizes.isEmpty()) {
					size = Integer.MAX_VALUE;
					for (String s : sizes.split(",")) {
						size = Math.min(size, Integer.parseInt(s));
					}
				}
				Structure cs = VariableUtilities.findOrCreateClassStruct((GhidraClass) ns, dtm);
				if (cs.isZeroLength() || cs.getLength() < size) {
					cs.growStructure(size - (cs.isZeroLength() ? 0 : cs.getLength()));
				}
				DataType vt = dtm.getDataType("/DRA2/vtables/" + name + "_vftable");
				if (vt == null) {
					vt = dtm.getDataType("/DRA2/vtables/" + original + "_vftable");
				}
				if (vt != null) {
					cs.replaceAtOffset(0, new PointerDataType(vt), 4, "vftable", "vtable at " + c[1]);
				}
				structs++;
			}
		}
		println("class structs rebuilt: " + structs);

		int fixed = 0;
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			if (!(f.getParentNamespace() instanceof GhidraClass)) {
				continue;
			}
			// constructors/destructors moved into a class kept an unknown convention; MSVC uses __thiscall for them
			if ("unknown".equals(f.getCallingConventionName()) || f.getCallingConventionName() == null) {
				f.setCallingConvention("__thiscall");
				fixed++;
				continue;
			}
			if (!"__thiscall".equals(f.getCallingConventionName())) {
				continue;
			}
			if (f.getParameterCount() == 0) {
				continue;
			}
			Parameter p0 = f.getParameter(0);
			if (!p0.isAutoParameter() && p0.isRegisterVariable() && p0.getRegister() != null &&
				p0.getRegister().getName().equals("ECX")) {
				f.removeParameter(0);
				fixed++;
			}
		}
		println(String.format("renamed %d classes, gave %d methods an automatic 'this'", renamed, fixed));
	}
}
