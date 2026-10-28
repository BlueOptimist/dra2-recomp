// Recovers C++ classes without RTTI.
//  1. vtables: constants stored to [reg+0] (MOV dword ptr [ECX], vtable) that point at a run of code pointers in .rdata
//  2. object sizes: PUSH size / CALL operator_new / ... / CALL <vtable writer>
//  3. inheritance: a writer that calls another class's writer before storing its own vtable derives from it
//  4. Ghidra: class namespace + class struct (size, vftable field) + vtable struct; virtual methods moved into the
//     class that introduces them, so the decompiler types 'this'.
// Args: <classes.tsv> [apply]   ("apply" writes to the program; otherwise report only)
// @category Digimon
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.CParserUtils;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.CategoryPath;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeConflictHandler;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.Structure;
import ghidra.program.model.data.StructureDataType;
import ghidra.program.model.data.VoidDataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.GhidraClass;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.VariableUtilities;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.Varnode;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.SymbolTable;

public class RecoverClasses extends GhidraScript {
	private static final long OPERATOR_NEW = 0x1c82a0L;
	private static final long LIB_START = 0x1b2790L, LIB_END = 0x1c8200L, CRT_START = 0x1dd3a0L;
	private static final long XAPI_START = 0x1256f0L, XAPI_END = 0x12f600L;

	private Memory mem;
	private MemoryBlock text, rdata;

	private boolean isLibraryAddr(long a) {
		return (a >= LIB_START && a < LIB_END) || (a >= CRT_START && a < 0x215890L) || (a >= XAPI_START && a < XAPI_END);
	}

	private long readPtr(Address a) {
		try {
			return mem.getInt(a) & 0xFFFFFFFFL;
		}
		catch (Exception e) {
			return -1;
		}
	}

	private boolean isCodePtr(long v) {
		if (v < 0) {
			return false;
		}
		Address a = toAddr(v);
		return text.contains(a) && getInstructionAt(a) != null;
	}

	static class Vt {
		long addr;
		List<Long> slots = new ArrayList<>();
		Set<Long> writers = new TreeSet<>();
		Set<Integer> sizes = new TreeSet<>();
		Set<Long> parents = new TreeSet<>();
		String name;
	}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		boolean apply = args.length > 1 && args[1].equals("apply");
		mem = currentProgram.getMemory();
		text = mem.getBlock(".text");
		rdata = mem.getBlock(".rdata");
		if (args.length > 1 && args[1].equals("debug")) {
			Function f = getFunctionAt(toAddr(0x1552e0L));
			for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
				if (ins.toString().contains("2a4354")) {
					println(ins.getAddress() + " " + ins);
					for (PcodeOp op : ins.getPcode()) {
						println("   " + op);
					}
				}
			}
			println("text=" + text + " rdata=" + rdata + " isCodePtr(first slot)=" + isCodePtr(readPtr(toAddr(0x2a4354L))) +
				" ptr=" + Long.toHexString(readPtr(toAddr(0x2a4354L))));
			return;
		}

		// 1. vtable stores: STORE [register], constant -> .rdata run of code pointers
		Map<Long, Vt> vts = new TreeMap<>();
		Map<Long, List<long[]>> storesByFunc = new HashMap<>(); // func -> (instr addr, vtable)
		for (Instruction ins : currentProgram.getListing().getInstructions(text.getStart(), true)) {
			if (!text.contains(ins.getAddress())) {
				break;
			}
			Map<Long, Long> uniqueConst = new HashMap<>(); // raw pcode routes immediates through a unique temporary
			for (PcodeOp op : ins.getPcode()) {
				if (op.getOpcode() == PcodeOp.COPY && op.getOutput() != null && op.getOutput().isUnique() &&
					op.getInput(0).isConstant()) {
					uniqueConst.put(op.getOutput().getOffset(), op.getInput(0).getOffset());
					continue;
				}
				if (op.getOpcode() != PcodeOp.STORE) {
					continue;
				}
				Varnode ptr = op.getInput(1), val = op.getInput(2);
				if (!ptr.isRegister() || val.getSize() != 4) {
					continue;
				}
				Long cval = val.isConstant() ? Long.valueOf(val.getOffset())
						: val.isUnique() ? uniqueConst.get(val.getOffset()) : null;
				if (cval == null) {
					continue;
				}
				final long vtAddr = cval;
				Address target = toAddr(vtAddr);
				if (!rdata.contains(target) || !isCodePtr(readPtr(target))) {
					continue;
				}
				Function f = getFunctionContaining(ins.getAddress());
				if (f == null || isLibraryAddr(f.getEntryPoint().getOffset())) {
					continue;
				}
				Vt vt = vts.computeIfAbsent(vtAddr, k -> {
					Vt n = new Vt();
					n.addr = k;
					return n;
				});
				vt.writers.add(f.getEntryPoint().getOffset());
				storesByFunc.computeIfAbsent(f.getEntryPoint().getOffset(), k -> new ArrayList<>())
						.add(new long[] { ins.getAddress().getOffset(), vtAddr });
			}
		}
		// slots: consecutive code pointers until a non-code pointer or the next vtable
		for (Vt vt : vts.values()) {
			Address a = toAddr(vt.addr);
			while (rdata.contains(a)) {
				long p = readPtr(a);
				if (!isCodePtr(p) || (a.getOffset() != vt.addr && vts.containsKey(a.getOffset()))) {
					break;
				}
				vt.slots.add(p);
				a = a.add(4);
			}
		}
		println("vtables found: " + vts.size());

		Map<Long, Set<Long>> vtOfWriter = new HashMap<>();
		for (Vt vt : vts.values()) {
			for (long w : vt.writers) {
				vtOfWriter.computeIfAbsent(w, k -> new TreeSet<>()).add(vt.addr);
			}
		}

		// 2. sizes: PUSH imm; CALL operator_new; ... CALL writer (within 30 instructions)
		Function opNew = getFunctionAt(toAddr(OPERATOR_NEW));
		for (Function caller : opNew.getCallingFunctions(monitor)) {
			List<Instruction> list = new ArrayList<>();
			currentProgram.getListing().getInstructions(caller.getBody(), true).forEach(list::add);
			for (int i = 0; i < list.size(); i++) {
				Instruction ins = list.get(i);
				if (!ins.getFlowType().isCall() || ins.getFlows().length == 0 ||
					ins.getFlows()[0].getOffset() != OPERATOR_NEW) {
					continue;
				}
				Integer size = null;
				for (int b = i - 1; b >= Math.max(0, i - 4) && size == null; b--) {
					Instruction prev = list.get(b);
					if (prev.getMnemonicString().equals("PUSH") && prev.getScalar(0) != null) {
						size = (int) prev.getScalar(0).getUnsignedValue();
					}
				}
				if (size == null) {
					continue;
				}
				for (int k = i + 1; k < Math.min(list.size(), i + 30); k++) {
					Instruction nxt = list.get(k);
					if (nxt.getFlowType().isCall() && nxt.getFlows().length > 0) {
						Set<Long> v = vtOfWriter.get(nxt.getFlows()[0].getOffset());
						if (v != null) {
							for (long vaddr : v) {
								vts.get(vaddr).sizes.add(size);
							}
							break;
						}
						if (nxt.getFlows()[0].getOffset() == OPERATOR_NEW) {
							break;
						}
					}
				}
			}
		}

		// 3. inheritance: writer W calls writer W' (other vtable) before W's own store
		for (Map.Entry<Long, List<long[]>> e : storesByFunc.entrySet()) {
			Function w = getFunctionAt(toAddr(e.getKey()));
			for (long[] store : e.getValue()) {
				Vt own = vts.get(store[1]);
				for (Instruction ins : currentProgram.getListing().getInstructions(w.getBody(), true)) {
					if (ins.getAddress().getOffset() >= store[0]) {
						break;
					}
					if (ins.getFlowType().isCall() && ins.getFlows().length > 0) {
						Set<Long> base = vtOfWriter.get(ins.getFlows()[0].getOffset());
						if (base != null) {
							for (long b : base) {
								if (b != own.addr) {
									own.parents.add(b);
								}
							}
						}
					}
				}
			}
		}

		// names
		Map<Long, String> known = new HashMap<>();
		known.put(0x14e4f0L, "DebugMenuItem");
		for (Vt vt : vts.values()) {
			vt.name = null;
			for (long w : vt.writers) {
				if (known.containsKey(w) && vt.writers.size() <= 3) {
					vt.name = known.get(w);
				}
			}
			if (vt.name == null) {
				vt.name = String.format("Class_%06x", vt.addr);
			}
		}

		// which class introduces each method: a function in several vtables goes to the class whose vtable it is in
		// that has no ancestor also containing it
		Map<Long, Set<Long>> methodVts = new HashMap<>();
		for (Vt vt : vts.values()) {
			for (long s : vt.slots) {
				methodVts.computeIfAbsent(s, k -> new TreeSet<>()).add(vt.addr);
			}
		}
		Map<Long, Long> owner = new HashMap<>();
		int shared = 0;
		for (Map.Entry<Long, Set<Long>> e : methodVts.entrySet()) {
			if (isLibraryAddr(e.getKey())) {
				continue;
			}
			List<Long> roots = new ArrayList<>();
			for (long v : e.getValue()) {
				boolean ancestorHasIt = false;
				Set<Long> seen = new HashSet<>();
				List<Long> stack = new ArrayList<>(vts.get(v).parents);
				while (!stack.isEmpty()) {
					long p = stack.remove(stack.size() - 1);
					if (!seen.add(p)) {
						continue;
					}
					if (e.getValue().contains(p)) {
						ancestorHasIt = true;
						break;
					}
					Vt pv = vts.get(p);
					if (pv != null) {
						stack.addAll(pv.parents);
					}
				}
				if (!ancestorHasIt) {
					roots.add(v);
				}
			}
			if (roots.size() == 1) {
				owner.put(e.getKey(), roots.get(0));
			}
			else {
				shared++;
			}
		}
		println("methods with a single owning class: " + owner.size() + ", shared by unrelated vtables: " + shared);

		// report
		int withSize = 0, withParent = 0;
		try (PrintWriter out = new PrintWriter(args[0])) {
			out.println("class\tvtable\tslots\tsize\tparents\twriters\towned_methods");
			for (Vt vt : vts.values()) {
				long ownedCount = owner.values().stream().filter(v -> v == vt.addr).count();
				if (!vt.sizes.isEmpty()) {
					withSize++;
				}
				if (!vt.parents.isEmpty()) {
					withParent++;
				}
				List<String> ps = new ArrayList<>();
				for (long p : vt.parents) {
					ps.add(vts.get(p).name);
				}
				List<String> ws = new ArrayList<>();
				for (long w : vt.writers) {
					ws.add(Long.toHexString(w));
				}
				out.println(vt.name + "\t" + Long.toHexString(vt.addr) + "\t" + vt.slots.size() + "\t" + vt.sizes + "\t" +
					String.join(",", ps) + "\t" + String.join(",", ws) + "\t" + ownedCount);
			}
		}
		println("classes with known size: " + withSize + ", with a parent: " + withParent);
		if (!apply) {
			return;
		}

		// 4. apply to the program
		DataTypeManager dtm = currentProgram.getDataTypeManager();
		SymbolTable st = currentProgram.getSymbolTable();
		Map<Long, GhidraClass> classes = new HashMap<>();
		for (Vt vt : vts.values()) {
			Namespace existing = st.getNamespace(vt.name, currentProgram.getGlobalNamespace());
			GhidraClass cls = existing instanceof GhidraClass ? (GhidraClass) existing
					: st.createClass(currentProgram.getGlobalNamespace(), vt.name, SourceType.USER_DEFINED);
			classes.put(vt.addr, cls);
			// vtable struct
			StructureDataType vtType = new StructureDataType(new CategoryPath("/DRA2/vtables"), vt.name + "_vftable", 0, dtm);
			for (int i = 0; i < vt.slots.size(); i++) {
				vtType.add(new PointerDataType(VoidDataType.dataType), 4, String.format("slot%02d_%06x", i, vt.slots.get(i)), null);
			}
			DataType vtResolved = dtm.resolve(vtType, DataTypeConflictHandler.REPLACE_HANDLER);
			// class struct: size and vftable pointer
			Structure cs = VariableUtilities.findOrCreateClassStruct(cls, dtm);
			int size = vt.sizes.isEmpty() ? 4 : vt.sizes.iterator().next();
			if (vt.sizes.size() > 1) {
				size = ((TreeSet<Integer>) vt.sizes).last();
			}
			if (cs.getLength() < size) {
				cs.growStructure(size - (cs.isZeroLength() ? 0 : cs.getLength()));
			}
			cs.replaceAtOffset(0, new PointerDataType(vtResolved), 4, "vftable", "vtable at " + Long.toHexString(vt.addr));
			createLabel(toAddr(vt.addr), vt.name + "_vftable", true, SourceType.USER_DEFINED);
		}
		int moved = 0;
		for (Map.Entry<Long, Long> e : owner.entrySet()) {
			Function f = getFunctionAt(toAddr(e.getKey()));
			if (f == null) {
				f = createFunction(toAddr(e.getKey()), null);
			}
			if (f == null || !(f.getParentNamespace().isGlobal())) {
				continue;
			}
			f.setParentNamespace(classes.get(e.getValue()));
			if (!"__thiscall".equals(f.getCallingConventionName())) {
				f.setCallingConvention("__thiscall");
			}
			moved++;
		}
		// writers (constructors/destructors) go to their class too, when they write exactly one vtable
		int writers = 0;
		for (Map.Entry<Long, Set<Long>> e : vtOfWriter.entrySet()) {
			if (e.getValue().size() != 1) {
				continue;
			}
			Function f = getFunctionAt(toAddr(e.getKey()));
			if (f != null && f.getParentNamespace().isGlobal()) {
				f.setParentNamespace(classes.get(e.getValue().iterator().next()));
				writers++;
			}
		}
		println("applied: " + classes.size() + " classes, " + moved + " virtual methods, " + writers + " ctors/dtors moved");
	}
}
