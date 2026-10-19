// Applies checked names from a TSV (addr, name, evidence), e.g. the ones tools/ghidra/port_names.py
// collects from port/source. A name only replaces Ghidra's own (FUN_*, LAB_*, analysis names); a
// different hand-made name already there is reported, never overwritten. Code with no function yet
// (overlay programs, code only reached through tables) becomes a function. The evidence goes in a
// PRE comment ("sh23ds: <evidence>") so the listing points back at the port code.
// Args: <names.tsv> [report.txt]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

public class ApplyNames extends GhidraScript {
  public void run() throws Exception {
    String[] args = getScriptArgs();
    FunctionManager fm = currentProgram.getFunctionManager();
    SymbolTable st = currentProgram.getSymbolTable();
    Listing listing = currentProgram.getListing();
    Memory mem = currentProgram.getMemory();
    int named = 0, same = 0, made = 0, labels = 0;
    List<String> conflicts = new ArrayList<>();
    for (String line : Files.readAllLines(Paths.get(args[0]), StandardCharsets.UTF_8)) {
      if (line.isEmpty() || line.startsWith("addr\t") || line.startsWith("#")) continue;
      String[] p = line.split("\t", -1);
      Address a = toAddr(p[0]);
      String name = p[1], why = p.length > 2 ? p[2] : "";
      MemoryBlock blk = mem.getBlock(a);
      if (blk == null) { conflicts.add(p[0] + "\t" + name + "\tnot mapped"); continue; }
      Function f = fm.getFunctionAt(a);
      if (f == null && blk.isExecute()) {
        if (fm.getFunctionContaining(a) != null) {
          conflicts.add(p[0] + "\t" + name + "\tinside " + fm.getFunctionContaining(a).getName() + " (not an entry)");
          continue;
        }
        if (listing.getInstructionAt(a) == null) disassemble(a);
        f = createFunction(a, null);
        if (f != null) made++;
      }
      Symbol s = f != null ? f.getSymbol() : st.getPrimarySymbol(a);
      if (s != null && s.getName().equals(name)) same++;
      else if (s == null || s.getSource() == SourceType.DEFAULT || s.getSource() == SourceType.ANALYSIS) {
        if (f != null) f.setName(name, SourceType.USER_DEFINED);
        else { st.createLabel(a, name, SourceType.USER_DEFINED).setPrimary(); labels++; }
        named++;
      } else {
        conflicts.add(p[0] + "\t" + name + "\talready " + s.getName());
        continue;
      }
      if (!why.isEmpty()) {
        String note = "sh23ds: " + why;
        String pre = listing.getComment(CodeUnit.PRE_COMMENT, a);
        if (pre == null || !pre.contains(note)) listing.setComment(a, CodeUnit.PRE_COMMENT, pre == null ? note : pre + "\n" + note);
      }
    }
    if (args.length > 1) Files.write(Paths.get(args[1]), conflicts, StandardCharsets.UTF_8);
    println("named " + named + " (" + labels + " labels, " + made + " new functions), already right " + same + ", conflicts " + conflicts.size());
    for (String c : conflicts) println("conflict " + c);
  }
}
