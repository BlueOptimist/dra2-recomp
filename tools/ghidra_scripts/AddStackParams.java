// Declares the stack arguments a function reads but its signature doesn't have. The decompiler shows them as
// in_stack_00000004 (the caller's argument area above the return address); recompiled, they are a stand-in area of
// garbage and callers pass nothing there. Common in tail-jump wrappers (mov ecx, [ecx+60h]; jmp f), which have no
// ret for the purge check to see.
// For each row of analysis/stack_params.tsv (address, name, highest in_stack offset, from tools/find_stack_params.py):
// keep the parameters where they are (custom storage) and append undefined4 stack parameters up to that offset.
// The stack purge stays as it is.
// Args: <stack_params.tsv>
// @category Digimon
import java.io.BufferedReader;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.Undefined4DataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Function.FunctionUpdateType;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.listing.ReturnParameterImpl;
import ghidra.program.model.listing.VariableStorage;
import ghidra.program.model.symbol.SourceType;

public class AddStackParams extends GhidraScript {
	@Override
	protected void run() throws Exception {
		int functions = 0, added = 0, failed = 0;
		try (BufferedReader in = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				String[] c = line.split("\t");
				Function f = getFunctionAt(toAddr(Long.parseLong(c[0], 16)));
				if (f == null) {
					failed++;
					continue;
				}
				int highest = Integer.parseInt(c[2], 16);
				try {
					int purge = f.getStackPurgeSize();
					List<Parameter> params = new ArrayList<>();
					int stackEnd = 4;
					for (Parameter p : f.getParameters()) {
						VariableStorage st = p.getVariableStorage();
						params.add(new ParameterImpl(p.getName(), p.getDataType(), st, currentProgram));
						if (st.isStackStorage()) {
							stackEnd = Math.max(stackEnd, st.getStackOffset() + st.size());
						}
					}
					int before = params.size(), k = params.size();
					while (stackEnd <= highest) {
						params.add(new ParameterImpl("param_" + (++k), Undefined4DataType.dataType,
							new VariableStorage(currentProgram, stackEnd, 4), currentProgram));
						stackEnd += 4;
					}
					if (params.size() == before) {
						continue;
					}
					ReturnParameterImpl ret = new ReturnParameterImpl(f.getReturn().getDataType(),
						f.getReturn().getVariableStorage(), currentProgram);
					f.updateFunction(f.getCallingConventionName(), ret, params, FunctionUpdateType.CUSTOM_STORAGE,
						true, SourceType.USER_DEFINED);
					if (purge >= 0) {
						f.setStackPurgeSize(purge);
					}
					added += params.size() - before;
					functions++;
				}
				catch (Exception e) {
					printerr(c[0] + " " + c[1] + ": " + e.getMessage());
					failed++;
				}
			}
		}
		println(String.format("stack parameters added: %d to %d functions, failed: %d", added, functions, failed));
	}
}
