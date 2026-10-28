// Retypes struct fields that hold x87 floats but were inferred as integers/pointers.
// Evidence from p-code: a LOAD from struct+offset whose value feeds a float operation, or a STORE to struct+offset of
// a value produced by one. The address must be PTRSUB(<pointer to structure>, <constant offset>).
// A field is retyped only if it got float evidence and is never dereferenced as a pointer.
// Only functions listed with errors in compile_report.tsv are scanned (the retyped structs apply everywhere).
// Args: <compile_report.tsv> <report.tsv>
// @category Digimon
import java.io.BufferedReader;
import java.io.FileReader;
import java.io.PrintWriter;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.AbstractFloatDataType;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeComponent;
import ghidra.program.model.data.DoubleDataType;
import ghidra.program.model.data.FloatDataType;
import ghidra.program.model.data.Pointer;
import ghidra.program.model.data.Structure;
import ghidra.program.model.data.TypeDef;
import ghidra.program.model.listing.Function;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.PcodeOpAST;
import ghidra.program.model.pcode.Varnode;

public class FixFloatFields extends GhidraScript {
	private static final Set<Integer> FLOAT_OUT = Set.of(PcodeOp.FLOAT_ADD, PcodeOp.FLOAT_SUB, PcodeOp.FLOAT_MULT,
		PcodeOp.FLOAT_DIV, PcodeOp.FLOAT_NEG, PcodeOp.FLOAT_ABS, PcodeOp.FLOAT_SQRT, PcodeOp.FLOAT_INT2FLOAT,
		PcodeOp.FLOAT_FLOAT2FLOAT, PcodeOp.FLOAT_CEIL, PcodeOp.FLOAT_FLOOR, PcodeOp.FLOAT_ROUND);
	private static final Set<Integer> FLOAT_IN = Set.of(PcodeOp.FLOAT_ADD, PcodeOp.FLOAT_SUB, PcodeOp.FLOAT_MULT,
		PcodeOp.FLOAT_DIV, PcodeOp.FLOAT_NEG, PcodeOp.FLOAT_ABS, PcodeOp.FLOAT_SQRT, PcodeOp.FLOAT_LESS,
		PcodeOp.FLOAT_LESSEQUAL, PcodeOp.FLOAT_EQUAL, PcodeOp.FLOAT_NOTEQUAL, PcodeOp.FLOAT_NAN,
		PcodeOp.FLOAT_FLOAT2FLOAT, PcodeOp.FLOAT_TRUNC, PcodeOp.FLOAT_CEIL, PcodeOp.FLOAT_FLOOR, PcodeOp.FLOAT_ROUND);

	/** (structure, offset) for an address computed as PTRSUB(struct pointer, const), else null. */
	private Object[] fieldOf(Varnode addr) {
		PcodeOp def = addr.getDef();
		if (def == null || def.getOpcode() != PcodeOp.PTRSUB || !def.getInput(1).isConstant() ||
			def.getInput(0).getHigh() == null) {
			return null;
		}
		DataType t = def.getInput(0).getHigh().getDataType();
		if (t instanceof TypeDef) {
			t = ((TypeDef) t).getBaseDataType();
		}
		if (!(t instanceof Pointer)) {
			return null;
		}
		DataType target = ((Pointer) t).getDataType();
		if (target instanceof TypeDef) {
			target = ((TypeDef) target).getBaseDataType();
		}
		if (!(target instanceof Structure)) {
			return null;
		}
		return new Object[] { target, (int) def.getInput(1).getOffset() };
	}

	/** does this value (through copies/casts) reach a float operation, or get dereferenced? */
	private int[] uses(Varnode v) {
		int floats = 0, derefs = 0;
		Deque<Varnode> work = new ArrayDeque<>();
		Set<Varnode> seen = new HashSet<>();
		work.add(v);
		while (!work.isEmpty()) {
			Varnode x = work.poll();
			if (!seen.add(x)) {
				continue;
			}
			for (Iterator<PcodeOp> it = x.getDescendants(); it.hasNext();) {
				PcodeOp op = it.next();
				int oc = op.getOpcode();
				if (FLOAT_IN.contains(oc)) {
					floats++;
				}
				else if ((oc == PcodeOp.LOAD || oc == PcodeOp.STORE) && op.getInput(1) == x) {
					derefs++;
				}
				else if ((oc == PcodeOp.PTRSUB || oc == PcodeOp.PTRADD || oc == PcodeOp.CALLIND) && op.getInput(0) == x) {
					derefs++;
				}
				else if ((oc == PcodeOp.COPY || oc == PcodeOp.CAST || oc == PcodeOp.MULTIEQUAL) && op.getOutput() != null) {
					work.add(op.getOutput());
				}
			}
		}
		return new int[] { floats, derefs };
	}

	private boolean producedByFloat(Varnode v) {
		PcodeOp def = v.getDef();
		for (int depth = 0; def != null && depth < 4; depth++) {
			if (FLOAT_OUT.contains(def.getOpcode())) {
				return true;
			}
			if (def.getOpcode() != PcodeOp.COPY && def.getOpcode() != PcodeOp.CAST) {
				return false;
			}
			def = def.getInput(0).getDef();
		}
		return false;
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
		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		// evidence per (struct, offset): [float votes, pointer votes, size]
		Map<String, int[]> evidence = new HashMap<>();
		Map<String, Structure> structs = new HashMap<>();
		for (Function f : todo) {
			monitor.checkCancelled();
			DecompileResults r = di.decompileFunction(f, 30, monitor);
			HighFunction hf = r.decompileCompleted() ? r.getHighFunction() : null;
			if (hf == null) {
				continue;
			}
			for (Iterator<PcodeOpAST> it = hf.getPcodeOps(); it.hasNext();) {
				PcodeOpAST op = it.next();
				Varnode addr, value;
				boolean isFloat = false, isPtr = false;
				if (op.getOpcode() == PcodeOp.LOAD) {
					addr = op.getInput(1);
					value = op.getOutput();
					int[] u = uses(value);
					isFloat = u[0] > 0;
					isPtr = u[1] > 0;
				}
				else if (op.getOpcode() == PcodeOp.STORE) {
					addr = op.getInput(1);
					value = op.getInput(2);
					isFloat = producedByFloat(value);
				}
				else {
					continue;
				}
				Object[] fld = fieldOf(addr);
				if (fld == null || (value.getSize() != 4 && value.getSize() != 8)) {
					continue;
				}
				Structure s = (Structure) fld[0];
				String key = s.getPathName() + "@" + fld[1];
				structs.put(key, s);
				int[] e = evidence.computeIfAbsent(key, k -> new int[3]);
				if (isFloat) {
					e[0]++;
				}
				if (isPtr) {
					e[1]++;
				}
				e[2] = value.getSize();
			}
		}
		di.dispose();
		int retyped = 0;
		List<String> report = new ArrayList<>();
		for (Map.Entry<String, int[]> e : evidence.entrySet()) {
			int[] v = e.getValue();
			if (v[0] == 0 || v[1] > 0) {
				continue;
			}
			Structure s = structs.get(e.getKey());
			int off = Integer.parseInt(e.getKey().substring(e.getKey().lastIndexOf('@') + 1));
			DataTypeComponent c = s.getComponentContaining(off);
			if (c == null || c.getOffset() != off || c.getDataType() instanceof AbstractFloatDataType) {
				continue;
			}
			DataType ft = v[2] == 8 ? DoubleDataType.dataType : FloatDataType.dataType;
			if (off + ft.getLength() > s.getLength()) {
				continue;
			}
			try {
				String name = c.getFieldName();
				s.replaceAtOffset(off, ft, ft.getLength(), name, "float (inferred from x87 use)");
				report.add(s.getName() + "\t" + Integer.toHexString(off) + "\t" + name + "\t" + ft.getName() + "\t" + v[0]);
				retyped++;
			}
			catch (Exception ex) {
				// overlapping components: leave as is
			}
		}
		try (PrintWriter out = new PrintWriter(args[1])) {
			out.println("struct\toffset\tfield\ttype\tfloat_uses");
			report.forEach(out::println);
		}
		println(String.format("fields seen %d, retyped to float/double %d", evidence.size(), retyped));
	}
}
