// Makes each listed function's stack purge match its machine code. A function whose "ret N" disagrees with its
// signature (a cdecl one that ends in ret 8, a __thiscall one with a stack parameter that ends in a plain ret)
// throws the decompiler's stack tracking off in every caller: after the call its stack offsets are shifted, so one
// variable gets two names and calls are shown with the wrong arguments.
// For each row of analysis/purge_mismatches.tsv (tools/check_purges.py): keep the parameters where they are
// (custom storage, so the convention can't move them), append undefined4 stack parameters when the code pops more
// than the signature has, and set the stack purge to the code's value.
// Args: <purge_mismatches.tsv>
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

public class FixPurges extends GhidraScript {
	@Override
	protected void run() throws Exception {
		int fixed = 0, added = 0, failed = 0;
		try (BufferedReader in = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				String[] c = line.split("\t");
				Function f = getFunctionAt(toAddr(Long.parseLong(c[0], 16)));
				if (f == null) {
					failed++;
					continue;
				}
				int pop = Integer.parseInt(c[4]);
				try {
					// current parameters with their current storage
					List<Parameter> params = new ArrayList<>();
					int stackEnd = 4; // first stack argument at entry ESP + 4
					for (Parameter p : f.getParameters()) {
						VariableStorage st = p.getVariableStorage();
						params.add(new ParameterImpl(p.getName(), p.getDataType(), st, currentProgram));
						if (st.isStackStorage()) {
							stackEnd = Math.max(stackEnd, st.getStackOffset() + st.size());
						}
					}
					// the code pops arguments the signature doesn't have
					int k = params.size();
					while (stackEnd - 4 < pop) {
						params.add(new ParameterImpl("param_" + (++k), Undefined4DataType.dataType,
							new VariableStorage(currentProgram, stackEnd, 4), currentProgram));
						stackEnd += 4;
						added++;
					}
					ReturnParameterImpl ret = new ReturnParameterImpl(f.getReturn().getDataType(),
						f.getReturn().getVariableStorage(), currentProgram);
					f.updateFunction(f.getCallingConventionName(), ret, params, FunctionUpdateType.CUSTOM_STORAGE,
						true, SourceType.USER_DEFINED);
					f.setStackPurgeSize(pop);
					fixed++;
				}
				catch (Exception e) {
					printerr(c[0] + " " + c[1] + ": " + e.getMessage());
					failed++;
				}
			}
		}
		println(String.format("stack purges set: %d (%d parameters added), failed: %d", fixed, added, failed));
	}
}
