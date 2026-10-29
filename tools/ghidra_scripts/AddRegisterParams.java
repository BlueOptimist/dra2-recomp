// Turns register inputs no standard convention passes (EAX, EDX: MSVC's internal helpers and inlined-call leftovers)
// into explicit parameters. The decompiler shows them as in_EAX / in_EDX, which recompiled are uninitialised locals.
// Each listed register becomes a leading parameter with custom storage in that register, the existing parameters keep
// their storage; the decompiler then shows the value every caller leaves in the register as an ordinary argument.
// The recompiled callers and callee use the same C prototype, so passing it on the stack there is consistent.
// Reads analysis/register_params.tsv (address, registers like "EAX" or "EAX,EDX"), from find_register_inputs.py.
// Args: <register_params.tsv>
// @category Digimon
import java.io.BufferedReader;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.Undefined4DataType;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Function.FunctionUpdateType;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.listing.ReturnParameterImpl;
import ghidra.program.model.listing.VariableStorage;
import ghidra.program.model.symbol.SourceType;

public class AddRegisterParams extends GhidraScript {
	@Override
	protected void run() throws Exception {
		int changed = 0, skipped = 0;
		try (BufferedReader in = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				String[] c = line.split("\t");
				if (c.length < 2) {
					continue;
				}
				Function f = getFunctionAt(toAddr(Long.parseLong(c[0], 16)));
				if (f == null) {
					skipped++;
					continue;
				}
				List<Parameter> params = new ArrayList<>();
				List<String> have = new ArrayList<>();
				for (Parameter p : f.getParameters()) {
					if (p.isRegisterVariable() && p.getRegister() != null) {
						have.add(p.getRegister().getName());
					}
				}
				for (String reg : c[1].split(",")) {
					if (have.contains(reg)) {
						continue;
					}
					Register r = currentProgram.getRegister(reg);
					params.add(new ParameterImpl("in_" + reg.toLowerCase(), Undefined4DataType.dataType,
						new VariableStorage(currentProgram, r), currentProgram));
				}
				if (params.isEmpty()) {
					skipped++;
					continue;
				}
				for (Parameter p : f.getParameters()) {
					params.add(new ParameterImpl(p.getName(), p.getDataType(), p.getVariableStorage(), currentProgram));
				}
				ReturnParameterImpl ret = new ReturnParameterImpl(f.getReturnType(), f.getReturn().getVariableStorage(),
					currentProgram);
				f.updateFunction(f.getCallingConventionName(), ret, params, FunctionUpdateType.CUSTOM_STORAGE, true,
					SourceType.USER_DEFINED);
				changed++;
			}
		}
		println("register parameters added to " + changed + " functions, skipped " + skipped);
	}
}
