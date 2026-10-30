// Makes the stack purge Ghidra holds for each function match its machine code. The exported C doesn't show the
// purge, and a convention can hide one (FUN_001a1e70 was __stdcall with an unknown purge, so callers assumed it
// popped 8 bytes; its code ends in a plain ret and the caller cleans up). In game_main that put one struct under two
// names 8 bytes apart, and its first field was never initialised.
// Reads analysis/code_pops.tsv (address, name, the pop of every ret in the function: tools/check_purges.py). Where
// the database's purge differs (or is unknown), the parameters stay where they are (custom storage), missing stack
// parameters are added, and the purge is set from the code.
// Args: <code_pops.tsv>
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

public class ApplyCodePops extends GhidraScript {
	@Override
	protected void run() throws Exception {
		int checked = 0, fixed = 0, added = 0, failed = 0;
		try (BufferedReader in = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				String[] c = line.split("\t");
				Function f = getFunctionAt(toAddr(Long.parseLong(c[0], 16)));
				if (f == null || f.isThunk() || f.hasVarArgs()) {
					continue;
				}
				checked++;
				int pop = Integer.parseInt(c[2]);
				int purge = f.getStackPurgeSize();
				// Every stack argument takes a whole 4-byte slot (a char parameter at Stack[0x4] still ends at 8).
				// Parameters an earlier run added after a narrow one, at an unaligned offset, overlap it: they are
				// dropped here and added again on the boundary.
				List<Parameter> params = new ArrayList<>();
				int stackEnd = 4;
				boolean misplaced = false;
				for (Parameter p : f.getParameters()) {
					VariableStorage st = p.getVariableStorage();
					if (st.isStackStorage() && st.getStackOffset() % 4 != 0) {
						misplaced = true;
						continue;
					}
					params.add(new ParameterImpl(p.getName(), p.getDataType(), st, currentProgram));
					if (st.isStackStorage()) {
						stackEnd = Math.max(stackEnd, (st.getStackOffset() + st.size() + 3) & ~3);
					}
				}
				// With custom storage, an "unknown" convention makes the decompiler ignore the parameters (callers pass
				// nothing, the body reads in_stack_00000004): a real one is named, from the code's pop.
				String cc = f.getCallingConventionName();
				boolean unnamed = cc == null || cc.equals("unknown") || cc.equals("default");
				if (unnamed) {
					cc = pop == 0 ? "__cdecl" : "__stdcall";
				}
				if (!misplaced && !unnamed && purge == pop && (pop == 0 || stackEnd - 4 >= pop)) {
					continue;
				}
				try {
					int k = params.size();
					while (stackEnd - 4 < pop) {
						params.add(new ParameterImpl("param_" + (++k), Undefined4DataType.dataType,
							new VariableStorage(currentProgram, stackEnd, 4), currentProgram));
						stackEnd += 4;
						added++;
					}
					ReturnParameterImpl ret = new ReturnParameterImpl(f.getReturn().getDataType(),
						f.getReturn().getVariableStorage(), currentProgram);
					f.updateFunction(cc, ret, params, FunctionUpdateType.CUSTOM_STORAGE, true, SourceType.USER_DEFINED);
					f.setStackPurgeSize(pop);
					fixed++;
				}
				catch (Exception e) {
					printerr(c[0] + " " + c[1] + ": " + e.getMessage());
					failed++;
				}
			}
		}
		println(String.format("purges checked: %d, set from the code: %d (%d parameters added), failed: %d", checked,
			fixed, added, failed));
	}
}
