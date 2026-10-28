// Imports decomp/dra2_formats.h into the program, types known engine functions, and propagates resource struct
// types into parameters: Res_Find(name, tag, ...) results that are passed to a function make that parameter a
// pointer to the tag's struct, then typed parameters passed onward do the same (a few rounds).
// A parameter is only typed when every observed flow agrees on its struct.
// Args: <dra2_formats.h> <report.tsv>
// @category Digimon
import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayDeque;
import java.util.Deque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.CParser;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeConflictHandler;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.FunctionDefinition;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.Structure;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.HighFunctionDBUtil;
import ghidra.program.model.pcode.HighParam;
import ghidra.program.model.pcode.HighSymbol;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.PcodeOpAST;
import ghidra.program.model.pcode.Varnode;
import ghidra.program.model.symbol.SourceType;

public class TypeEngine extends GhidraScript {
	private static final long RES_FIND = 0x133d50L, RES_FIND_NEXT = 0x1b0ee0L;
	private static final long LIB_START = 0x1b2790L, LIB_END = 0x1c8200L, CRT_START = 0x1dd3a0L;

	private static final String[][] TAG_TYPES = { { "lua!", "LuaChunk" }, { "banm", "Animation" }, { "body", "Body" },
		{ "bmsh", "SkinnedMesh" }, { "rdms", "StaticMesh" }, { "surf", "Surface" }, { "gshd", "Shader" },
		{ "wave", "Wave" }, { "levl", "Level" }, { "node", "Node" }, { "musc", "Music" }, { "indx", "ResIndex" } };

	private static final String[][] SIGNATURES = {
		{ "00156770", "void __cdecl Res_ValidateHeader(char *filename, ResHeader *header);" },
		{ "00133d50", "void * __cdecl Res_Find(char *name, fourcc tag, void *prev);" },
		{ "001b0ee0", "void * __cdecl Res_FindNextOfType(fourcc tag, void *prev);" },
		{ "001c82a0", "void * __cdecl operator_new(unsigned int size);" },
		{ "00175b70", "int __cdecl Lua_RunScriptResource(void *L, char *name);" },
		{ "001b3460", "int __cdecl luaL_loadbuffer(void *L, char *buffer, unsigned int size, char *name);" },
	};
	// FatalErrorf is only renamed and marked noreturn: giving it a varargs signature makes the decompiler drop the
	// format arguments at call sites, which are the most useful part of those calls.

	private DataTypeManager dtm;
	private DecompInterface di;
	private final Map<Long, DataType> tagStruct = new HashMap<>();

	private static long fourccValue(String t) {
		byte[] b = t.getBytes();
		return (b[0] & 0xFFL) | (b[1] & 0xFFL) << 8 | (b[2] & 0xFFL) << 16 | (b[3] & 0xFFL) << 24;
	}

	private boolean isLibrary(Function f) {
		long a = f.getEntryPoint().getOffset();
		return (a >= LIB_START && a < LIB_END) || a >= CRT_START || f.getName().startsWith("lua");
	}

	private HighFunction decompile(Function f) {
		DecompileResults r = di.decompileFunction(f, 60, monitor);
		return r.decompileCompleted() ? r.getHighFunction() : null;
	}

	// Follow a value through copies/casts/phi nodes; record (callee, param index) for every call it is passed to.
	private void flowsInto(Varnode start, DataType type, Map<String, Set<String>> votes) {
		Deque<Varnode> work = new ArrayDeque<>();
		Set<Varnode> seen = new HashSet<>();
		work.add(start);
		while (!work.isEmpty()) {
			Varnode v = work.poll();
			if (!seen.add(v)) {
				continue;
			}
			Iterator<PcodeOp> it = v.getDescendants();
			while (it.hasNext()) {
				PcodeOp op = it.next();
				switch (op.getOpcode()) {
					case PcodeOp.COPY:
					case PcodeOp.CAST:
					case PcodeOp.MULTIEQUAL:
						if (op.getOutput() != null) {
							work.add(op.getOutput());
						}
						break;
					case PcodeOp.CALL:
						Function callee = getFunctionAt(op.getInput(0).getAddress());
						if (callee == null || isLibrary(callee) || callee.getEntryPoint().getOffset() == RES_FIND ||
							callee.getEntryPoint().getOffset() == RES_FIND_NEXT) {
							break;
						}
						for (int i = 1; i < op.getNumInputs(); i++) {
							if (op.getInput(i) == v) {
								votes.computeIfAbsent(callee.getEntryPoint() + ":" + (i - 1), k -> new HashSet<>())
										.add(type.getName());
							}
						}
						break;
					default:
						break;
				}
			}
		}
	}

	private static final java.util.regex.Pattern FORMAT_SPEC =
		java.util.regex.Pattern.compile("%[-+ #0]*(\\*|\\d+)?(\\.(\\*|\\d+))?[hlLI]*[a-zA-Z%]");

	private String cString(long addr) {
		try {
			StringBuilder sb = new StringBuilder();
			for (int i = 0; i < 512; i++) {
				byte c = currentProgram.getMemory().getByte(toAddr(addr + i));
				if (c == 0) {
					return sb.toString();
				}
				sb.append((char) (c & 0xFF));
			}
		}
		catch (Exception e) {
			// not a readable string
		}
		return null;
	}

	private long constantOf(Varnode v) {
		if (v.isConstant()) {
			return v.getOffset();
		}
		PcodeOp def = v.getDef();
		if (def != null && (def.getOpcode() == PcodeOp.PTRSUB || def.getOpcode() == PcodeOp.COPY ||
			def.getOpcode() == PcodeOp.CAST)) {
			long sum = 0;
			for (int i = 0; i < def.getNumInputs(); i++) {
				if (!def.getInput(i).isConstant()) {
					return -1;
				}
				sum += def.getInput(i).getOffset();
			}
			return sum;
		}
		return -1;
	}

	// For each call to a printf-style variadic function, write a call-site prototype with one parameter per format
	// specifier (what Ghidra's variadic signature analyzer does), so the decompiler shows the real arguments.
	private int overrideFormatCalls(Function target) throws Exception {
		int count = 0;
		for (Function caller : target.getCallingFunctions(monitor)) {
			HighFunction hf = decompile(caller);
			if (hf == null) {
				continue;
			}
			Iterator<PcodeOpAST> ops = hf.getPcodeOps();
			while (ops.hasNext()) {
				PcodeOpAST op = ops.next();
				if (op.getOpcode() != PcodeOp.CALL || op.getNumInputs() < 2 ||
					!op.getInput(0).getAddress().equals(target.getEntryPoint())) {
					continue;
				}
				long fmtAddr = constantOf(op.getInput(1));
				String fmt = fmtAddr < 0 ? null : cString(fmtAddr);
				if (fmt == null) {
					continue;
				}
				java.util.List<ghidra.program.model.data.ParameterDefinition> params = new java.util.ArrayList<>();
				params.add(new ghidra.program.model.data.ParameterDefinitionImpl("format",
					new PointerDataType(ghidra.program.model.data.CharDataType.dataType), null));
				java.util.regex.Matcher m = FORMAT_SPEC.matcher(fmt);
				while (m.find()) {
					String spec = m.group();
					if (spec.endsWith("%")) {
						continue;
					}
					for (char ch : spec.toCharArray()) {
						if (ch == '*') {
							params.add(new ghidra.program.model.data.ParameterDefinitionImpl("width",
								ghidra.program.model.data.IntegerDataType.dataType, null));
						}
					}
					char conv = spec.charAt(spec.length() - 1);
					DataType t = conv == 's' ? new PointerDataType(ghidra.program.model.data.CharDataType.dataType)
							: "eEfgG".indexOf(conv) >= 0 ? ghidra.program.model.data.DoubleDataType.dataType
							: ghidra.program.model.data.IntegerDataType.dataType;
					params.add(new ghidra.program.model.data.ParameterDefinitionImpl("arg" + params.size(), t, null));
				}
				ghidra.program.model.data.FunctionDefinitionDataType sig =
					new ghidra.program.model.data.FunctionDefinitionDataType(target.getName());
				sig.setArguments(params.toArray(new ghidra.program.model.data.ParameterDefinition[0]));
				sig.setReturnType(ghidra.program.model.data.VoidDataType.dataType);
				sig.setCallingConvention("__cdecl");
				sig.setNoReturn(true);
				HighFunctionDBUtil.writeOverride(caller, op.getSeqnum().getTarget(), sig);
				count++;
			}
		}
		return count;
	}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		dtm = currentProgram.getDataTypeManager();

		// 1. Import the structs (preprocessor lines stripped; the header needs no packing).
		StringBuilder src = new StringBuilder();
		for (String line : Files.readAllLines(Path.of(args[0]))) {
			if (!line.trim().startsWith("#")) {
				src.append(line).append('\n');
			}
		}
		CParser parser = new CParser(dtm, true, null);
		parser.parse(src.toString());
		Map<String, DataType> byName = new HashMap<>();
		for (DataType dt : parser.getComposites().values()) {
			byName.put(dt.getName(), dtm.resolve(dt, DataTypeConflictHandler.REPLACE_HANDLER));
		}
		for (Map.Entry<String, DataType> e : parser.getTypes().entrySet()) {
			dtm.resolve(e.getValue(), DataTypeConflictHandler.REPLACE_HANDLER);
		}
		for (String[] chk : new String[][] { { "Bone", "160" }, { "SkinnedVertex", "60" }, { "StaticVertex", "44" },
			{ "ShaderLayer", "120" }, { "ResTocEntry", "20" }, { "MeshDescriptor", "44" } }) {
			DataType dt = byName.get(chk[0]);
			println(chk[0] + " size " + (dt == null ? "missing" : dt.getLength()) + " (expected " + chk[1] + ")");
		}
		for (String[] tt : TAG_TYPES) {
			tagStruct.put(fourccValue(tt[0]), byName.get(tt[1]));
		}

		// 2. Known signatures.
		CParser sigParser = new CParser(dtm, false, null);
		for (String[] s : SIGNATURES) {
			Function f = getFunctionAt(toAddr(Long.parseLong(s[0], 16)));
			DataType def = sigParser.parse(s[1]);
			if (f != null && def instanceof FunctionDefinition) {
				new ApplyFunctionSignatureCmd(f.getEntryPoint(), (FunctionDefinition) def, SourceType.USER_DEFINED)
						.applyTo(currentProgram, monitor);
			}
		}
		Function fatal = getFunctionAt(toAddr(0x1d4c70L));
		DataType fatalDef = sigParser.parse("void __cdecl FatalErrorf(char *format, ...);");
		new ApplyFunctionSignatureCmd(fatal.getEntryPoint(), (FunctionDefinition) fatalDef, SourceType.USER_DEFINED)
				.applyTo(currentProgram, monitor);
		fatal.setNoReturn(true);

		di = new DecompInterface();
		di.openProgram(currentProgram);
		println("printf-style call-site overrides for FatalErrorf: " + overrideFormatCalls(fatal));

		// 3. Seed: every Res_Find / Res_FindNextOfType call with a constant tag.
		Map<String, Set<String>> votes = new TreeMap<>();
		Set<Function> callers = new HashSet<>();
		for (long target : new long[] { RES_FIND, RES_FIND_NEXT }) {
			Function t = getFunctionAt(toAddr(target));
			callers.addAll(t.getCallingFunctions(monitor));
		}
		int seeds = 0;
		for (Function caller : callers) {
			HighFunction hf = decompile(caller);
			if (hf == null) {
				continue;
			}
			Iterator<PcodeOpAST> ops = hf.getPcodeOps();
			while (ops.hasNext()) {
				PcodeOpAST op = ops.next();
				if (op.getOpcode() != PcodeOp.CALL || op.getOutput() == null) {
					continue;
				}
				long tgt = op.getInput(0).getAddress().getOffset();
				int tagInput = tgt == RES_FIND ? 2 : tgt == RES_FIND_NEXT ? 1 : -1;
				if (tagInput < 0 || op.getNumInputs() <= tagInput || !op.getInput(tagInput).isConstant()) {
					continue;
				}
				DataType st = tagStruct.get(op.getInput(tagInput).getOffset());
				if (st != null) {
					seeds++;
					flowsInto(op.getOutput(), st, votes);
				}
			}
		}
		println("seed calls with known tags: " + seeds);

		// 3b. Type the caller's local variable that receives each Res_Find result (all assignments must agree).
		int typedLocals = 0;
		for (Function caller : callers) {
			if (isLibrary(caller)) {
				continue;
			}
			boolean changed = true;
			Set<String> done = new HashSet<>();
			while (changed) { // re-decompile after each update: HighSymbols go stale once the database changes
				changed = false;
				HighFunction hf = decompile(caller);
				if (hf == null) {
					break;
				}
				Map<HighSymbol, Set<DataType>> wanted = new HashMap<>();
				Iterator<PcodeOpAST> ops = hf.getPcodeOps();
				while (ops.hasNext()) {
					PcodeOpAST op = ops.next();
					if (op.getOpcode() != PcodeOp.CALL || op.getOutput() == null) {
						continue;
					}
					long tgt = op.getInput(0).getAddress().getOffset();
					int tagInput = tgt == RES_FIND ? 2 : tgt == RES_FIND_NEXT ? 1 : -1;
					if (tagInput < 0 || op.getNumInputs() <= tagInput || !op.getInput(tagInput).isConstant()) {
						continue;
					}
					DataType st = tagStruct.get(op.getInput(tagInput).getOffset());
					if (st == null || op.getOutput().getHigh() == null) {
						continue;
					}
					HighSymbol sym = op.getOutput().getHigh().getSymbol();
					if (sym != null && !sym.isParameter() && !sym.isGlobal()) {
						wanted.computeIfAbsent(sym, k -> new HashSet<>()).add(st);
					}
				}
				for (Map.Entry<HighSymbol, Set<DataType>> w : wanted.entrySet()) {
					String key = w.getKey().getName();
					if (w.getValue().size() != 1 || done.contains(key)) {
						continue;
					}
					DataType ptr = new PointerDataType(w.getValue().iterator().next());
					try {
						HighFunctionDBUtil.updateDBVariable(w.getKey(), null, ptr, SourceType.USER_DEFINED);
						done.add(key);
						typedLocals++;
						changed = true;
						break;
					}
					catch (Exception ex) {
						done.add(key);
					}
				}
			}
		}
		println("typed local variables holding resources: " + typedLocals);

		// 4. Apply unanimous votes, then follow newly typed parameters onward for a few rounds.
		Map<String, String> applied = new TreeMap<>();
		Map<String, String> thisTypes = new TreeMap<>();
		int conflicts = 0;
		for (int round = 1; round <= 3 && !votes.isEmpty(); round++) {
			Map<String, Set<String>> next = new TreeMap<>();
			for (Map.Entry<String, Set<String>> e : votes.entrySet()) {
				if (e.getValue().size() != 1) {
					conflicts++;
					continue;
				}
				if (applied.containsKey(e.getKey())) {
					continue;
				}
				String[] k = e.getKey().split(":");
				Function f = getFunctionAt(toAddr(Long.parseLong(k[0], 16)));
				int idx = Integer.parseInt(k[1]);
				DataType st = byName.get(e.getValue().iterator().next());
				HighFunction hf = decompile(f);
				if (hf == null) {
					continue;
				}
				if (f.getSignatureSource() == SourceType.DEFAULT) {
					HighFunctionDBUtil.commitParamsToDatabase(hf, true, HighFunctionDBUtil.ReturnCommitOption.NO_COMMIT,
						SourceType.ANALYSIS);
				}
				Parameter p = idx < f.getParameterCount() ? f.getParameter(idx) : null;
				if (p != null && p.isAutoParameter()) {
					thisTypes.put(e.getKey(), st.getName()); // a __thiscall 'this': handled as a class below
					continue;
				}
				if (p == null) {
					continue;
				}
				try {
					p.setDataType(new PointerDataType(st), SourceType.USER_DEFINED);
				}
				catch (Exception ex) {
					println("skip " + e.getKey() + ": " + ex.getMessage());
					continue;
				}
				applied.put(e.getKey(), st.getName());
				// follow this parameter into the callee's own calls
				HighFunction typed = decompile(f);
				if (typed == null) {
					continue;
				}
				for (int s = 0; s < typed.getLocalSymbolMap().getNumParams(); s++) {
					HighSymbol sym = typed.getLocalSymbolMap().getParamSymbol(s);
					if (sym != null && sym.getHighVariable() instanceof HighParam &&
						((HighParam) sym.getHighVariable()).getSlot() == idx) {
						for (Varnode inst : sym.getHighVariable().getInstances()) {
							flowsInto(inst, st, next);
						}
					}
				}
			}
			println("round " + round + ": typed parameters " + applied.size() + ", 'this' candidates " + thisTypes.size() +
				", conflicting " + conflicts);
			votes = next;
		}
		for (Map.Entry<String, String> e : thisTypes.entrySet()) {
			println("  this-candidate " + e.getKey() + " " + getFunctionAt(toAddr(Long.parseLong(e.getKey().split(":")[0], 16))).getName() + " -> " + e.getValue());
		}

		try (PrintWriter out = new PrintWriter(args[1])) {
			out.println("function\tname\tparam\ttype");
			for (Map.Entry<String, String> e : applied.entrySet()) {
				String[] k = e.getKey().split(":");
				Address a = toAddr(Long.parseLong(k[0], 16));
				out.println(k[0] + "\t" + getFunctionAt(a).getName() + "\t" + k[1] + "\t" + e.getValue() + " *");
			}
		}
		di.dispose();
	}
}
