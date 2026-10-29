// Applies identified C runtime signatures from analysis/crt_signatures.tsv (address, signature, storage, evidence):
// the name, return type, parameters and calling convention come from the C signature. A storage column such as
// "params=ST1,ST0;return=ST0" or "params=ST0;return=EDX:EAX" puts parameters and return value in those registers
// (custom storage; "stack+N" is the stack slot at entry ESP + N). That's how MSVC's x87 helpers (_ftol, _CIpow, _CIatan2, ...) take their operands; with it the
// decompiler shows the arguments at every call site instead of losing them on the FPU stack.
// Args: <crt_signatures.tsv>
// @category Digimon
import java.io.BufferedReader;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.CParser;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.FunctionDefinition;
import ghidra.program.model.data.ParameterDefinition;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Function.FunctionUpdateType;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.listing.ReturnParameterImpl;
import ghidra.program.model.listing.VariableStorage;
import ghidra.program.model.symbol.SourceType;

public class ApplyCrtSignatures extends GhidraScript {
	// "EAX", "EDX:EAX", or "stack+8" (the stack slot at entry ESP + 8, as wide as the parameter)
	private VariableStorage storage(String spec, int size) throws Exception {
		if (spec.startsWith("stack+")) {
			return new VariableStorage(currentProgram, Integer.parseInt(spec.substring(6)), size);
		}
		String[] regs = spec.split(":");
		Register[] r = new Register[regs.length];
		for (int i = 0; i < regs.length; i++) {
			r[i] = currentProgram.getRegister(regs[i]);
			if (r[i] == null) {
				throw new IllegalArgumentException("unknown register " + regs[i]);
			}
		}
		return new VariableStorage(currentProgram, r);
	}

	@Override
	protected void run() throws Exception {
		CParser parser = new CParser(currentProgram.getDataTypeManager(), false, null);
		int applied = 0, custom = 0;
		try (BufferedReader in = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				String[] c = line.split("\t", -1);
				if (c.length < 2 || c[1].isEmpty()) {
					continue;
				}
				Function f = getFunctionAt(toAddr(Long.parseLong(c[0], 16)));
				if (f == null) {
					printerr("no function at " + c[0]);
					continue;
				}
				if (f.isThunk()) {
					f = f.getThunkedFunction(true);
				}
				// CParser has no __thiscall: parse without it, set it after
				boolean thiscall = c[1].contains("__thiscall");
				DataType def = parser.parse(c[1].replace("__thiscall", ""));
				if (!(def instanceof FunctionDefinition)) {
					printerr("could not parse " + c[1]);
					continue;
				}
				FunctionDefinition fd = (FunctionDefinition) def;
				new ApplyFunctionSignatureCmd(f.getEntryPoint(), fd, SourceType.USER_DEFINED).applyTo(currentProgram,
					monitor);
				f.setName(fd.getName(), SourceType.USER_DEFINED);
				if (thiscall) {
					f.setCallingConvention("__thiscall");
				}
				applied++;
				if (c.length > 2 && !c[2].isEmpty()) {
					String paramSpec = "", retSpec = "";
					for (String part : c[2].split(";")) {
						if (part.startsWith("params=")) {
							paramSpec = part.substring(7);
						}
						else if (part.startsWith("return=")) {
							retSpec = part.substring(7);
						}
					}
					String[] ps = paramSpec.isEmpty() ? new String[0] : paramSpec.split(",");
					ParameterDefinition[] args = fd.getArguments();
					List<Parameter> params = new ArrayList<>();
					for (int i = 0; i < args.length; i++) {
						params.add(new ParameterImpl(args[i].getName(), args[i].getDataType(),
							storage(ps[i], args[i].getDataType().getLength()), currentProgram));
					}
					ReturnParameterImpl ret = new ReturnParameterImpl(fd.getReturnType(),
						storage(retSpec, fd.getReturnType().getLength()), currentProgram);
					f.updateFunction(f.getCallingConventionName(), ret, params, FunctionUpdateType.CUSTOM_STORAGE,
						true, SourceType.USER_DEFINED);
					custom++;
				}
				println(c[0] + " " + f.getCallingConventionName() + " " + f.getSignature().getPrototypeString() +
					(f.hasCustomVariableStorage() ? "   [" + c[2] + "]" : ""));
			}
		}
		println("applied " + applied + " signatures, " + custom + " with x87/register storage");
	}
}
