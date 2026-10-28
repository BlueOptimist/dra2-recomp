// Names class methods structurally (only functions still called FUN_*):
//  - scalar deleting destructor: calls operator delete (0x1c8330) after testing (flags & 1) -> deleting_dtor
//  - the class function it calls first -> dtor
//  - other vtable writers (constructors/destructors from classes.tsv) -> ctor
//  - every other vtable slot -> vfNN, so an override has the same name as the base method in that slot
// Args: <classes.tsv>
// @category Digimon
import java.io.BufferedReader;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.GhidraClass;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.SourceType;

public class NameMethods extends GhidraScript {
	private static final long OPERATOR_DELETE = 0x1c8330L;

	private boolean renameable(Function f) {
		return f != null && f.getName().startsWith("FUN_") && f.getParentNamespace() instanceof GhidraClass;
	}

	private void rename(Function f, String name) throws Exception {
		try {
			f.setName(name, SourceType.ANALYSIS);
		}
		catch (Exception e) {
			f.setName(name + "_" + f.getEntryPoint(), SourceType.ANALYSIS); // name already used in this namespace
		}
	}

	private Function deletingDtorTarget(Function f) {
		// pattern: CALL dtor ... TEST byte ptr [...],0x1 ... CALL operator_delete
		boolean testsOne = false, deletes = false;
		Function firstCallee = null;
		for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
			if (ins.getMnemonicString().equals("TEST") && ins.getScalar(1) != null && ins.getScalar(1).getValue() == 1) {
				testsOne = true;
			}
			if (ins.getFlowType().isCall() && ins.getFlows().length > 0) {
				Address t = ins.getFlows()[0];
				if (t.getOffset() == OPERATOR_DELETE) {
					deletes = true;
				}
				else if (firstCallee == null) {
					Function c = getFunctionAt(t);
					if (c != null && c.isThunk()) {
						c = c.getThunkedFunction(true);
					}
					firstCallee = c;
				}
			}
		}
		return testsOne && deletes && f.getBody().getNumAddresses() < 80 ? (firstCallee == null ? f : firstCallee) : null;
	}

	@Override
	protected void run() throws Exception {
		int dd = 0, dt = 0, ct = 0, vf = 0;
		List<String[]> rows = new ArrayList<>();
		try (BufferedReader in = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
			in.readLine();
			for (String line; (line = in.readLine()) != null;) {
				rows.add(line.split("\t"));
			}
		}
		Set<Long> dtors = new HashSet<>();
		// pass 1: deleting destructors and destructors from vtable slots
		for (String[] r : rows) {
			long vt = Long.parseLong(r[1], 16);
			int slots = Integer.parseInt(r[2]);
			for (int i = 0; i < slots; i++) {
				Function f = getFunctionAt(toAddr(getInt(toAddr(vt + 4L * i)) & 0xFFFFFFFFL));
				if (!renameable(f) && !(f != null && f.getName().equals("deleting_dtor"))) {
					continue;
				}
				Function target = deletingDtorTarget(f);
				if (target == null) {
					continue;
				}
				if (renameable(f)) {
					rename(f, "deleting_dtor");
					dd++;
				}
				if (target != f && renameable(target)) {
					rename(target, "dtor");
					dtors.add(target.getEntryPoint().getOffset());
					dt++;
				}
			}
		}
		// pass 2: remaining vtable writers are constructors
		for (String[] r : rows) {
			if (r.length < 6 || r[5].isEmpty()) {
				continue;
			}
			for (String w : r[5].split(",")) {
				Function f = getFunctionAt(toAddr(Long.parseLong(w, 16)));
				if (renameable(f) && !dtors.contains(f.getEntryPoint().getOffset())) {
					rename(f, "ctor");
					ct++;
				}
			}
		}
		// pass 3: slot names
		for (String[] r : rows) {
			long vt = Long.parseLong(r[1], 16);
			int slots = Integer.parseInt(r[2]);
			for (int i = 0; i < slots; i++) {
				Function f = getFunctionAt(toAddr(getInt(toAddr(vt + 4L * i)) & 0xFFFFFFFFL));
				if (renameable(f)) {
					rename(f, String.format("vf%02d", i));
					vf++;
				}
			}
		}
		println(String.format("named %d deleting destructors, %d destructors, %d constructors, %d virtual slots", dd, dt, ct, vf));
	}
}
