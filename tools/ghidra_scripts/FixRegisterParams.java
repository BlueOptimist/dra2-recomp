// Cleans up the EAX/EDX parameters AddRegisterParams.java added.
// 1. Some were never inputs: the body only keeps the register's upper bytes for a bool result,
//    CONCAT31((int3)((uint)in_eax >> 8), flag). These are removed (analysis/false_register_params.tsv, written by
//    tools/find_register_inputs.py).
// 2. In a __thiscall function the register parameter was listed before "this". Recompiled, the first C parameter
//    of a __thiscall function is the one in ECX, so a call through a vtable (object in ECX) handed the object
//    to the EAX parameter and read "this" from a stack slot. The ECX parameter is moved first; storage stays
//    custom, so the decompiler shows each register's value in its own position.
// Args: <false_register_params.tsv> <register_params.tsv>
// @category Digimon
import java.io.BufferedReader;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Function.FunctionUpdateType;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.listing.ReturnParameterImpl;
import ghidra.program.model.symbol.SourceType;

public class FixRegisterParams extends GhidraScript {
	private Set<String> read(String path, int column) throws Exception {
		Set<String> out = new HashSet<>();
		try (BufferedReader in = new BufferedReader(new FileReader(path))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				String[] c = line.split("\t");
				if (c.length > column) {
					out.add(c[0] + (column > 0 ? ":" + c[column] : ""));
				}
			}
		}
		return out;
	}

	private static String register(Parameter p) {
		Register r = p.getRegister();
		return r == null ? null : r.getName().toLowerCase();
	}

	@Override
	protected void run() throws Exception {
		Set<String> falseParams = read(getScriptArgs()[0], 2); // "address:in_eax"
		Set<String> candidates = read(getScriptArgs()[1], 0);
		for (String fp : falseParams) {
			candidates.add(fp.split(":")[0]);
		}
		int removed = 0, reordered = 0, touched = 0;
		for (String a : candidates) {
			Function f = getFunctionAt(toAddr(Long.parseLong(a, 16)));
			if (f == null || !f.hasCustomVariableStorage()) {
				continue;
			}
			List<Parameter> params = new ArrayList<>();
			Parameter ecx = null;
			boolean changed = false;
			for (Parameter p : f.getParameters()) {
				String reg = register(p);
				if (reg != null && (reg.equals("eax") || reg.equals("edx")) && falseParams.contains(a + ":in_" + reg)) {
					removed++;
					changed = true;
					continue;
				}
				if ("ecx".equals(reg) && ecx == null) {
					ecx = p;
					continue;
				}
				params.add(new ParameterImpl(p.getName(), p.getDataType(), p.getVariableStorage(), currentProgram));
			}
			if (ecx != null) {
				if (f.getParameter(0) == null || !"ecx".equals(register(f.getParameter(0)))) {
					reordered++;
					changed = true;
				}
				params.add(0, new ParameterImpl(ecx.getName(), ecx.getDataType(), ecx.getVariableStorage(),
					currentProgram));
			}
			if (!changed) {
				continue;
			}
			ReturnParameterImpl ret = new ReturnParameterImpl(f.getReturn().getDataType(),
				f.getReturn().getVariableStorage(), currentProgram);
			f.updateFunction(f.getCallingConventionName(), ret, params, FunctionUpdateType.CUSTOM_STORAGE, true,
				SourceType.USER_DEFINED);
			touched++;
		}
		println(String.format("register parameters: %d false ones removed, %d functions with this moved first, " +
			"%d functions updated", removed, reordered, touched));
	}
}
