// Removes every call-site signature override (ApplyCallOverrides.java writes them), so a corrected list can be
// applied from scratch: an override that is no longer listed would otherwise stay.
// @category Digimon
import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

public class ClearCallOverrides extends GhidraScript {
	@Override
	protected void run() throws Exception {
		SymbolTable st = currentProgram.getSymbolTable();
		int removed = 0;
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			Namespace ns = st.getNamespace("override", f);
			if (ns == null) {
				continue;
			}
			List<Symbol> symbols = new ArrayList<>();
			st.getSymbols(ns).forEach(symbols::add);
			for (Symbol s : symbols) {
				s.delete();
				removed++;
			}
			Symbol nsSymbol = ns.getSymbol();
			if (nsSymbol != null) {
				nsSymbol.delete();
			}
		}
		println("call overrides removed: " + removed);
	}
}
