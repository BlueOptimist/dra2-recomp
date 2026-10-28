// Defines functions Ghidra's analysis missed: every entry of a vtable-like run in .rdata
// (3+ consecutive pointers into .text, at least one of them a known function) that points
// at code outside any function, plus any addresses given as arguments.
// Args: [<hex address> ...]
// @category Digimon
import java.util.ArrayList;
import java.util.List;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class DefineMissingFunctions extends GhidraScript {
	private MemoryBlock text;
	private FunctionManager fm;

	private boolean inText(long v) {
		return v >= text.getStart().getOffset() && v < text.getEnd().getOffset();
	}

	private boolean define(Address a) throws Exception {
		if (fm.getFunctionContaining(a) != null) {
			return false;
		}
		if (getInstructionAt(a) == null) {
			DisassembleCommand cmd = new DisassembleCommand(a, null, true);
			cmd.applyTo(currentProgram, monitor);
		}
		Function f = createFunction(a, null);
		if (f == null) {
			printerr("could not create function at " + a);
			return false;
		}
		return true;
	}

	@Override
	protected void run() throws Exception {
		Memory mem = currentProgram.getMemory();
		text = mem.getBlock(".text");
		MemoryBlock rdata = mem.getBlock(".rdata");
		fm = currentProgram.getFunctionManager();
		int created = 0;

		for (String arg : getScriptArgs()) {
			if (define(toAddr(Long.parseLong(arg, 16)))) {
				created++;
			}
		}

		List<Long> run = new ArrayList<>();
		long start = rdata.getStart().getOffset() & ~3L;
		long end = rdata.getEnd().getOffset();
		int fromVtables = 0;
		for (long p = start; p <= end + 4; p += 4) {
			long v = p <= end - 3 ? (mem.getInt(toAddr(p)) & 0xffffffffL) : 0;
			if (p <= end - 3 && inText(v)) {
				run.add(v);
				continue;
			}
			if (run.size() >= 3) {
				boolean known = false;
				for (long t : run) {
					if (fm.getFunctionAt(toAddr(t)) != null) {
						known = true;
						break;
					}
				}
				if (known) {
					for (long t : run) {
						if (define(toAddr(t))) {
							fromVtables++;
						}
					}
				}
			}
			run.clear();
		}
		// pointers stored in .rdata/.data to code no function covers: callbacks and factory functions that only data
		// tables reference. A target counts when it starts after padding or a RET; the MSVC exception-unwind
		// funclets (0x1ee000-0x1f9da0) are left alone, a port doesn't use them.
		int fromData = 0;
		for (String name : new String[] { ".rdata", ".data" }) {
			MemoryBlock b = mem.getBlock(name);
			if (b == null || !b.isInitialized()) {
				continue;
			}
			for (long p = b.getStart().getOffset(); p + 4 <= b.getEnd().getOffset() + 1; p += 4) {
				long v = mem.getInt(toAddr(p)) & 0xffffffffL;
				if (!inText(v) || (v >= 0x1ee000L && v < 0x1f9da0L)) {
					continue;
				}
				Address a = toAddr(v);
				if (fm.getFunctionContaining(a) != null) {
					continue;
				}
				int prev = mem.getByte(a.subtract(1)) & 0xff;
				if (prev != 0x90 && prev != 0xcc && prev != 0xc3 && (v & 0xf) != 0) {
					continue;
				}
				if (define(a)) {
					fromData++;
				}
			}
		}
		println("defined " + created + " functions from arguments, " + fromVtables + " from vtable entries, " +
			fromData + " from other pointers in data");
	}
}
