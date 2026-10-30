// Gives __thiscall functions back their "this". Some ended up with custom storage and no parameter in ECX (the
// convention's automatic "this" is gone then), so the body reads the object as in_ECX, an uninitialised local once
// recompiled: Class_296960::vf21, a float getter called while drawing, read garbage + 0xac.
// For each address in analysis/missing_this.tsv (tools/find_register_inputs.py): insert a pointer parameter "this"
// in ECX first, keep the other parameters where they are and keep the stack purge.
// Args: <missing_this.tsv>
// @category Digimon
import java.io.BufferedReader;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Function.FunctionUpdateType;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.listing.ReturnParameterImpl;
import ghidra.program.model.listing.VariableStorage;
import ghidra.program.model.symbol.SourceType;

public class AddThisParam extends GhidraScript {
	@Override
	protected void run() throws Exception {
		Register ecx = currentProgram.getRegister("ECX");
		int added = 0, kept = 0, failed = 0;
		try (BufferedReader in = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				String[] c = line.split("\t");
				Function f = getFunctionAt(toAddr(Long.parseLong(c[0], 16)));
				if (f == null) {
					failed++;
					continue;
				}
				try {
					int purge = f.getStackPurgeSize();
					List<Parameter> params = new ArrayList<>();
					boolean hasEcx = false;
					for (Parameter p : f.getParameters()) {
						VariableStorage st = p.getVariableStorage();
						if (ecx.equals(p.getRegister())) {
							hasEcx = true;
						}
						params.add(new ParameterImpl(p.getName(), p.getDataType(), st, currentProgram));
					}
					if (hasEcx) {
						kept++;
						continue;
					}
					params.add(0, new ParameterImpl("this", PointerDataType.dataType,
						new VariableStorage(currentProgram, ecx), currentProgram));
					ReturnParameterImpl ret = new ReturnParameterImpl(f.getReturn().getDataType(),
						f.getReturn().getVariableStorage(), currentProgram);
					f.updateFunction("__thiscall", ret, params, FunctionUpdateType.CUSTOM_STORAGE, true,
						SourceType.USER_DEFINED);
					if (purge >= 0) {
						f.setStackPurgeSize(purge);
					}
					added++;
				}
				catch (Exception e) {
					printerr(c[0] + ": " + e.getMessage());
					failed++;
				}
			}
		}
		println(String.format("this added: %d, already had one in ECX: %d, failed: %d", added, kept, failed));
	}
}
