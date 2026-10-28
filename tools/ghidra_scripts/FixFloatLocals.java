// Retypes locals and parameters that hold x87 float values but were typed as integers/pointers.
// Evidence comes from the decompiler's p-code: a variable is a float if any instance is produced by a float operation
// or consumed by one, and no instance is used as a pointer (LOAD/STORE address, PTRADD/PTRSUB base, indirect call).
// Only functions listed with errors in compile_report.tsv are processed.
// Args: <compile_report.tsv> <report.tsv>
// @category Digimon
import java.io.BufferedReader;
import java.io.FileReader;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.AbstractFloatDataType;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DoubleDataType;
import ghidra.program.model.data.Float10DataType;
import ghidra.program.model.data.FloatDataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.HighFunctionDBUtil;
import ghidra.program.model.pcode.HighSymbol;
import ghidra.program.model.pcode.HighVariable;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.Varnode;
import ghidra.program.model.symbol.SourceType;

public class FixFloatLocals extends GhidraScript {
	private static final Set<Integer> FLOAT_OUT = Set.of(PcodeOp.FLOAT_ADD, PcodeOp.FLOAT_SUB, PcodeOp.FLOAT_MULT,
		PcodeOp.FLOAT_DIV, PcodeOp.FLOAT_NEG, PcodeOp.FLOAT_ABS, PcodeOp.FLOAT_SQRT, PcodeOp.FLOAT_INT2FLOAT,
		PcodeOp.FLOAT_FLOAT2FLOAT, PcodeOp.FLOAT_CEIL, PcodeOp.FLOAT_FLOOR, PcodeOp.FLOAT_ROUND);
	private static final Set<Integer> FLOAT_IN = Set.of(PcodeOp.FLOAT_ADD, PcodeOp.FLOAT_SUB, PcodeOp.FLOAT_MULT,
		PcodeOp.FLOAT_DIV, PcodeOp.FLOAT_NEG, PcodeOp.FLOAT_ABS, PcodeOp.FLOAT_SQRT, PcodeOp.FLOAT_LESS,
		PcodeOp.FLOAT_LESSEQUAL, PcodeOp.FLOAT_EQUAL, PcodeOp.FLOAT_NOTEQUAL, PcodeOp.FLOAT_NAN,
		PcodeOp.FLOAT_FLOAT2FLOAT, PcodeOp.FLOAT_TRUNC, PcodeOp.FLOAT_CEIL, PcodeOp.FLOAT_FLOOR, PcodeOp.FLOAT_ROUND);

	/** float type for the variable, or null if it isn't clearly a float. */
	private DataType floatTypeFor(HighVariable hv) {
		if (hv == null || hv.getDataType() instanceof AbstractFloatDataType) {
			return null;
		}
		int size = hv.getSize();
		if (size != 4 && size != 8 && size != 10) {
			return null;
		}
		boolean floatUse = false;
		for (Varnode inst : hv.getInstances()) {
			// definition side: through copies/casts back to a float-producing operation
			PcodeOp def = inst.getDef();
			for (int depth = 0; def != null && depth < 4; depth++) {
				if (FLOAT_OUT.contains(def.getOpcode())) {
					floatUse = true;
					break;
				}
				if (def.getOpcode() != PcodeOp.COPY && def.getOpcode() != PcodeOp.CAST) {
					break;
				}
				def = def.getInput(0).getDef();
			}
			// use side: through copies/casts (the decompiler shows these as "(float)var") to float operations
			java.util.ArrayDeque<Varnode> work = new java.util.ArrayDeque<>();
			Set<Varnode> seen = new HashSet<>();
			work.add(inst);
			while (!work.isEmpty()) {
				Varnode v = work.poll();
				if (!seen.add(v)) {
					continue;
				}
				for (Iterator<PcodeOp> it = v.getDescendants(); it.hasNext();) {
					PcodeOp op = it.next();
					int oc = op.getOpcode();
					if (FLOAT_IN.contains(oc)) {
						floatUse = true;
					}
					if ((oc == PcodeOp.LOAD || oc == PcodeOp.STORE) && op.getInput(1) == v) {
						return null; // dereferenced: a real pointer
					}
					if ((oc == PcodeOp.PTRADD || oc == PcodeOp.PTRSUB || oc == PcodeOp.CALLIND) && op.getInput(0) == v) {
						return null;
					}
					if ((oc == PcodeOp.COPY || oc == PcodeOp.CAST) && op.getOutput() != null) {
						work.add(op.getOutput());
					}
				}
			}
		}
		if (!floatUse) {
			return null;
		}
		return size == 4 ? FloatDataType.dataType : size == 8 ? DoubleDataType.dataType : Float10DataType.dataType;
	}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		List<Function> todo = new ArrayList<>();
		try (BufferedReader in = new BufferedReader(new FileReader(args[0]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				String[] c = line.split("\t");
				if (c.length > 2 && !c[2].equals("0")) {
					Function f = getFunctionAt(toAddr(Long.parseLong(c[0], 16)));
					if (f != null) {
						todo.add(f);
					}
				}
			}
		}
		println("functions with errors: " + todo.size());
		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		int locals = 0, params = 0, touched = 0;
		List<String> report = new ArrayList<>();
		for (Function f : todo) {
			monitor.checkCancelled();
			Set<String> done = new HashSet<>();
			boolean changed = true, any = false;
			for (int round = 0; changed && round < 40; round++) {
				changed = false;
				DecompileResults r = di.decompileFunction(f, 30, monitor);
				HighFunction hf = r.decompileCompleted() ? r.getHighFunction() : null;
				if (hf == null) {
					break;
				}
				for (Iterator<HighSymbol> it = hf.getLocalSymbolMap().getSymbols(); it.hasNext();) {
					HighSymbol sym = it.next();
					if (done.contains(sym.getName()) || sym.getName().equals("this")) {
						continue;
					}
					DataType t = floatTypeFor(sym.getHighVariable());
					if (t == null) {
						continue;
					}
					done.add(sym.getName());
					try {
						if (sym.isParameter()) {
							Parameter p = f.getParameter(sym.getCategoryIndex());
							if (p == null || p.isAutoParameter() || p.getLength() != t.getLength()) {
								continue;
							}
							p.setDataType(t, SourceType.USER_DEFINED);
							params++;
						}
						else {
							HighFunctionDBUtil.updateDBVariable(sym, null, t, SourceType.USER_DEFINED);
							locals++;
						}
						report.add(f.getEntryPoint() + "\t" + f.getName(true) + "\t" + sym.getName() + "\t" + t.getName() +
							"\t" + (sym.isParameter() ? "param" : "local"));
						changed = true;
						any = true;
						break; // symbols are stale after a database change: decompile again
					}
					catch (Exception e) {
						// leave it
					}
				}
			}
			if (any) {
				touched++;
			}
		}
		di.dispose();
		try (PrintWriter out = new PrintWriter(args[1])) {
			out.println("function\tname\tvariable\ttype\tkind");
			report.forEach(out::println);
		}
		println(String.format("retyped %d locals and %d parameters in %d functions", locals, params, touched));
	}
}
