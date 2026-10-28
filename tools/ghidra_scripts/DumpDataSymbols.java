// Exports what tools/gen_data.py needs to re-emit the XBE's data sections as relocatable data:
//   <out>/data_symbols.tsv   name, address, kind: every symbol (labels, vtables, strings, luakeys) in a
//                            non-executable block, plus every function entry (name with namespace, address)
//   <out>/data_refs.tsv      from, to: every memory reference whose source lies in a non-executable block
//                            (pointers stored in data, as found by Ghidra's analysis)
// Args: <out dir>
// @category Digimon
import java.io.File;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolType;

public class DumpDataSymbols extends GhidraScript {
	@Override
	protected void run() throws Exception {
		File out = new File(getScriptArgs()[0]);
		out.mkdirs();
		AddressSet data = new AddressSet();
		for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
			// .rdata is mapped executable by the XBE loader, so it is listed by name
			if (!b.isExecute() && b.isInitialized() || b.getName().equals(".data") || b.getName().equals(".rdata")) {
				data.add(b.getStart(), b.getEnd());
			}
		}
		int syms = 0, refs = 0;
		try (PrintWriter w = new PrintWriter(new File(out, "data_symbols.tsv"))) {
			w.println("name\taddress\tkind");
			for (Symbol s : currentProgram.getSymbolTable().getAllSymbols(true)) {
				Address a = s.getAddress();
				if (!a.isMemoryAddress()) {
					continue;
				}
				if (s.getSymbolType() == SymbolType.FUNCTION) {
					w.println(s.getName(true) + "\t" + a + "\tfunction");
					syms++;
				}
				else if (data.contains(a) && s.getSymbolType() == SymbolType.LABEL) {
					w.println(s.getName(true) + "\t" + a + "\tlabel");
					syms++;
				}
			}
		}
		try (PrintWriter w = new PrintWriter(new File(out, "data_refs.tsv"))) {
			w.println("from\tto");
			for (Address from : currentProgram.getReferenceManager().getReferenceSourceIterator(data, true)) {
				for (Reference r : currentProgram.getReferenceManager().getReferencesFrom(from)) {
					if (r.getToAddress().isMemoryAddress()) {
						w.println(from + "\t" + r.getToAddress());
						refs++;
					}
				}
			}
		}
		Function f = null;
		println("data/function symbols: " + syms + ", references from data: " + refs);
	}
}
