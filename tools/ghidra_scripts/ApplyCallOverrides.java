// Gives individual indirect calls the signature the decompiler couldn't see. Example: the stream loader pushes an
// allocation size once, before a branch, and both branches then call a different virtual method of the pool
// (mov al,[esi+110h]; test al,al; push edi; je ...; call [edx+0Ch]); the decompiler gives the push to one branch
// only, so the other call shows no argument and the method read a garbage size.
// Reads analysis/call_overrides.tsv: call instruction address, calling convention, return type, parameter types
// (comma separated; for __thiscall the first one is "this", in ECX), note. Types are builtin names: int, uint,
// void, pointer, undefined4 (a 4-byte value of unknown type: a float stays a bit copy).
// Args: <call_overrides.tsv>
// @category Digimon
import java.io.BufferedReader;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.FunctionDefinitionDataType;
import ghidra.program.model.data.IntegerDataType;
import ghidra.program.model.data.ParameterDefinition;
import ghidra.program.model.data.ParameterDefinitionImpl;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.Undefined4DataType;
import ghidra.program.model.data.UnsignedIntegerDataType;
import ghidra.program.model.data.VoidDataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.pcode.HighFunctionDBUtil;

public class ApplyCallOverrides extends GhidraScript {
	private DataType type(String name) {
		switch (name.trim()) {
			case "void": return VoidDataType.dataType;
			case "uint": return UnsignedIntegerDataType.dataType;
			case "pointer": return PointerDataType.dataType;
			case "undefined4": return Undefined4DataType.dataType;
			default: return IntegerDataType.dataType;
		}
	}

	@Override
	protected void run() throws Exception {
		int applied = 0, failed = 0;
		try (BufferedReader in = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				String[] c = line.split("\t");
				if (c.length < 4) {
					continue;
				}
				Address call = toAddr(Long.parseLong(c[0], 16));
				Function f = getFunctionContaining(call);
				if (f == null) {
					printerr("no function contains " + call);
					failed++;
					continue;
				}
				FunctionDefinitionDataType sig = new FunctionDefinitionDataType("override_" + c[0]);
				sig.setReturnType(type(c[2]));
				List<ParameterDefinition> params = new ArrayList<>();
				if (!c[3].trim().isEmpty()) {
					int k = 0;
					for (String p : c[3].split(",")) {
						params.add(new ParameterDefinitionImpl("arg" + (++k), type(p), null));
					}
				}
				sig.setArguments(params.toArray(new ParameterDefinition[0]));
				sig.setCallingConvention(c[1].trim());
				HighFunctionDBUtil.writeOverride(f, call, sig);
				applied++;
			}
		}
		println(String.format("call overrides applied: %d, failed: %d", applied, failed));
	}
}
