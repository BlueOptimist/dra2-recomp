// Commits a stable signature for every game/engine function that has none yet (signature source DEFAULT), so callers
// and the definition are decompiled against the same parameter list:
//  1. commit the decompiler's parameters and return type (inferred from the function's own body)
//  2. callee-cleanup functions ("RET n") must take exactly n bytes of stack arguments: pad with undefined4 parameters
//     or drop extras, and use __stdcall unless the function is already __thiscall
// Library code (Lua, CRT, XAPI) and the other XBE sections (D3D8, DSOUND, XGRPH, ...) are skipped unless "all" is
// given; game code calls into them, so their prototypes matter for the compile check too.
// Args: <report.tsv> [all]
// @category Digimon
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.Undefined4DataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.listing.Variable;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.HighFunctionDBUtil;
import ghidra.program.model.symbol.SourceType;

public class CommitSignatures extends GhidraScript {
	private static final long LIB_START = 0x1b2790L, LIB_END = 0x1c8200L, CRT_START = 0x1dd3a0L, CRT_END = 0x215890L;
	private static final long XAPI_START = 0x1256f0L, XAPI_END = 0x12f600L;

	private boolean isLibrary(long a) {
		return (a >= LIB_START && a < LIB_END) || (a >= CRT_START && a < CRT_END) || (a >= XAPI_START && a < XAPI_END);
	}

	private int retBytes(Function f) {
		int n = -1;
		for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
			if (ins.getMnemonicString().equals("RET")) {
				int v = ins.getNumOperands() > 0 && ins.getScalar(0) != null ? (int) ins.getScalar(0).getUnsignedValue() : 0;
				n = Math.max(n, v);
			}
		}
		return n; // -1: no RET (noreturn or tail jump)
	}

	private int stackParamBytes(Function f) {
		int bytes = 0;
		for (Parameter p : f.getParameters()) {
			if (!p.isAutoParameter() && p.isStackVariable()) {
				bytes += Math.max(4, (p.getLength() + 3) & ~3);
			}
		}
		return bytes;
	}

	@Override
	protected void run() throws Exception {
		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		int committed = 0, padded = 0, trimmed = 0, failed = 0, stdcall = 0;
		List<String> report = new ArrayList<>();
		boolean all = getScriptArgs().length > 1 && getScriptArgs()[1].equals("all");
		List<Function> todo = new ArrayList<>();
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			long a = f.getEntryPoint().getOffset();
			if (f.isThunk() || f.isExternal() || f.getSignatureSource() != SourceType.DEFAULT) {
				continue;
			}
			if (!all && (a < 0x11000L || a >= 0x217200L || isLibrary(a))) {
				continue;
			}
			todo.add(f);
		}
		println("functions without a committed signature: " + todo.size());
		int done = 0;
		for (Function f : todo) {
			monitor.checkCancelled();
			if (++done % 500 == 0) {
				println(done + "/" + todo.size());
			}
			DecompileResults r = di.decompileFunction(f, 30, monitor);
			HighFunction hf = r.decompileCompleted() ? r.getHighFunction() : null;
			if (hf == null) {
				failed++;
				continue;
			}
			try {
				HighFunctionDBUtil.commitParamsToDatabase(hf, true, HighFunctionDBUtil.ReturnCommitOption.COMMIT,
					SourceType.ANALYSIS);
				committed++;
			}
			catch (Exception e) {
				failed++;
				continue;
			}
			int ret = retBytes(f);
			if (ret <= 0) {
				continue;
			}
			// callee cleans the stack: the argument size is exactly 'ret' bytes
			String cc = f.getCallingConventionName();
			if (!"__thiscall".equals(cc) && !"__stdcall".equals(cc)) {
				f.setCallingConvention("__stdcall");
				stdcall++;
			}
			int have = stackParamBytes(f);
			if (have == ret) {
				continue;
			}
			List<Variable> params = new ArrayList<>();
			int bytes = 0;
			for (Parameter p : f.getParameters()) {
				if (p.isAutoParameter()) {
					continue;
				}
				if (p.isStackVariable()) {
					int sz = Math.max(4, (p.getLength() + 3) & ~3);
					if (bytes + sz > ret) {
						break; // drop parameters beyond the cleaned-up area
					}
					bytes += sz;
				}
				params.add(new ParameterImpl(p.getName(), p.getDataType(), currentProgram));
			}
			while (bytes < ret) {
				params.add(new ParameterImpl("param_" + (params.size() + 1), Undefined4DataType.dataType, currentProgram));
				bytes += 4;
			}
			try {
				f.replaceParameters(params, Function.FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, true,
					SourceType.ANALYSIS);
				if (have < ret) {
					padded++;
				}
				else {
					trimmed++;
				}
				report.add(f.getEntryPoint() + "\t" + f.getName(true) + "\t" + have + "\t" + ret);
			}
			catch (Exception e) {
				failed++;
			}
		}
		di.dispose();
		try (PrintWriter out = new PrintWriter(getScriptArgs()[0])) {
			out.println("function\tname\tstack_bytes_before\tret_bytes");
			report.forEach(out::println);
		}
		println(String.format("committed %d, set __stdcall on %d, padded %d, trimmed %d (RET n), failed %d",
			committed, stdcall, padded, trimmed, failed));
	}
}
