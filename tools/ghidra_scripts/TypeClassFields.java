// Types class fields and vtable slots.
//  1. For every class (namespace with a vftable label), run Ghidra's structure inference (FillOutStructureHelper, the
//     engine behind "Auto Fill in Structure") on the 'this' parameter of its constructors first, then its other
//     methods. Fields are filled into the class struct's undefined bytes with the size/type actually accessed.
//  2. Every vtable struct slot becomes a pointer to a function definition built from the slot function's signature.
// Args: none
// @category Digimon
import java.util.ArrayList;
import java.util.List;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.util.FillOutStructureHelper;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.CategoryPath;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeComponent;
import ghidra.program.model.data.DataTypeConflictHandler;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.FunctionDefinitionDataType;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.Structure;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.GhidraClass;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.HighSymbol;
import ghidra.program.model.pcode.HighVariable;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class TypeClassFields extends GhidraScript {
	@Override
	protected void run() throws Exception {
		DataTypeManager dtm = currentProgram.getDataTypeManager();
		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		FillOutStructureHelper fill = new FillOutStructureHelper(currentProgram, monitor);

		// group class functions: constructors first (they write every field), then the rest
		List<Function> ctors = new ArrayList<>(), others = new ArrayList<>();
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			Namespace ns = f.getParentNamespace();
			if (!(ns instanceof GhidraClass) || ns.getName().equals("XAPILIB") || f.getParameterCount() == 0 ||
				!f.getParameter(0).isAutoParameter()) {
				continue;
			}
			(f.getName().equals("ctor") ? ctors : others).add(f);
		}
		println("class functions: " + ctors.size() + " constructors, " + others.size() + " other methods");

		int processed = 0, filledBefore = countDefined(dtm);
		List<Function> all = new ArrayList<>(ctors);
		all.addAll(others);
		for (Function f : all) {
			monitor.checkCancelled();
			DecompileResults r = di.decompileFunction(f, 30, monitor);
			HighFunction hf = r.decompileCompleted() ? r.getHighFunction() : null;
			if (hf == null) {
				continue;
			}
			HighVariable thisVar = null;
			for (int i = 0; i < hf.getLocalSymbolMap().getNumParams(); i++) {
				HighSymbol s = hf.getLocalSymbolMap().getParamSymbol(i);
				if (s != null && s.getName().equals("this")) {
					thisVar = s.getHighVariable();
				}
			}
			if (thisVar == null) {
				continue;
			}
			try {
				// createNewStructure=false: merge discovered fields into the existing class struct
				fill.processStructure(thisVar, f, false, false, di);
				processed++;
			}
			catch (Exception e) {
				// leave this method's usage out
			}
			if (processed % 500 == 0 && processed > 0) {
				println("processed " + processed + "/" + all.size());
			}
		}
		println(String.format("structure inference run on %d methods; defined class-struct components %d -> %d",
			processed, filledBefore, countDefined(dtm)));

		// vtable slots -> function pointers
		CategoryPath sigCat = new CategoryPath("/DRA2/vtables/signatures");
		int slots = 0;
		SymbolIterator labels = currentProgram.getSymbolTable().getSymbolIterator("*_vftable", true);
		while (labels.hasNext()) {
			Symbol lbl = labels.next();
			String cls = lbl.getName().substring(0, lbl.getName().length() - "_vftable".length());
			DataType vt = dtm.getDataType("/DRA2/vtables/" + lbl.getName());
			if (!(vt instanceof Structure)) {
				continue;
			}
			Structure s = (Structure) vt;
			for (DataTypeComponent c : s.getComponents()) {
				long ptr = getInt(lbl.getAddress().add(c.getOffset())) & 0xFFFFFFFFL;
				Function f = getFunctionAt(toAddr(ptr));
				if (f == null) {
					continue;
				}
				FunctionDefinitionDataType def = new FunctionDefinitionDataType(f, false);
				def.setName(cls + "_" + c.getFieldName() + "_fn");
				def.setCategoryPath(sigCat);
				DataType resolved = dtm.resolve(def, DataTypeConflictHandler.REPLACE_HANDLER);
				s.replace(c.getOrdinal(), new PointerDataType(resolved), 4, c.getFieldName(), f.getName(true));
				slots++;
			}
		}
		println("vtable slots typed as function pointers: " + slots);
		di.dispose();
	}

	private int countDefined(DataTypeManager dtm) {
		int n = 0;
		for (Symbol s : currentProgram.getSymbolTable().getSymbolIterator("*_vftable", true)) {
			String cls = s.getName().substring(0, s.getName().length() - "_vftable".length());
			for (java.util.Iterator<Structure> it = dtm.getAllStructures(); it.hasNext();) {
				Structure st = it.next();
				if (st.getName().equals(cls) && st.getCategoryPath().isRoot()) {
					n += st.getNumDefinedComponents();
					break;
				}
			}
		}
		return n;
	}
}
