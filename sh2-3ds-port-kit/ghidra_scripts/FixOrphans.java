// Creates functions for orphan code in .text (instructions Ghidra disassembled but
// left outside any function), so D3D device users and SecuROM dispatcher sites get
// proper owners.
//
// Pass 1  code pointers: aligned dwords in initialized non-code blocks (vtables,
//         callback tables) that point at an orphan instruction become entries. A
//         pointer to undefined bytes is used only when it is 16-aligned, follows
//         padding or a return, and sits in a run of at least two code pointers.
// Pass 2  orphan runs: the first instruction of each contiguous orphan run becomes
//         an entry when it follows padding (CC/90) or a non-fallthrough instruction.
// Repeats pass 2 until nothing changes, then re-runs auto-analysis on the changes.
//
// Args: [report.txt] [watch addr ...]   default watch: g_pD3DDevice8, SecuROM dispatcher
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class FixOrphans extends GhidraScript {
  AddressSet text;
  Listing listing;
  FunctionManager fm;
  Set<Address> created = new HashSet<>();

  Function mk(Address a) throws Exception {
    Function f = createFunction(a, null);
    if (f != null) created.add(f.getEntryPoint());
    return f;
  }

  boolean orphan(Address a) {
    return listing.getInstructionAt(a) != null && fm.getFunctionContaining(a) == null;
  }

  boolean afterPaddingOrReturn(Address a) throws Exception {
    Address p = a.previous();
    if (p == null) return false;
    byte b = getByte(p);
    if (b == (byte) 0xCC || b == (byte) 0x90) return true;
    Instruction prev = listing.getInstructionContaining(p);
    return prev != null && !prev.hasFallthrough();
  }

  String counts(String[] watch) {
    int funcs = 0, orphanIns = 0;
    for (Function f : fm.getFunctions(text, true)) funcs++;
    for (Instruction i : listing.getInstructions(text, true)) if (fm.getFunctionContaining(i.getAddress()) == null) orphanIns++;
    StringBuilder sb = new StringBuilder("functions in .text " + funcs + ", orphan instructions " + orphanIns);
    ReferenceManager rm = currentProgram.getReferenceManager();
    for (String w : watch) {
      Set<Address> owners = new HashSet<>();
      int orphanRefs = 0;
      for (Reference r : rm.getReferencesTo(toAddr(w))) {
        Function f = fm.getFunctionContaining(r.getFromAddress());
        if (f == null) orphanRefs++; else owners.add(f.getEntryPoint());
      }
      sb.append("; ").append(w).append(": ").append(owners.size()).append(" functions + ").append(orphanRefs).append(" orphan sites");
    }
    return sb.toString();
  }

  public void run() throws Exception {
    String[] args = getScriptArgs();
    String report = args.length > 0 ? args[0] : null;
    String[] watch = args.length > 1 ? Arrays.copyOfRange(args, 1, args.length) : new String[] {"0xa35494", "0x24dc3f0"};
    listing = currentProgram.getListing();
    fm = currentProgram.getFunctionManager();
    Memory mem = currentProgram.getMemory();
    MemoryBlock tb = mem.getBlock(".text");
    text = new AddressSet(tb.getStart(), tb.getEnd());

    String before = counts(watch);
    println("before: " + before);

    // Pass 0: call targets in .text that were never disassembled, and functions with an
    // empty body (created on undefined bytes). Repeats while new code turns up.
    int p0 = 0, fixed = 0;
    for (int round = 0; round < 6 && !monitor.isCancelled(); round++) {
      int made = 0;
      List<Address> targets = new ArrayList<>();
      for (Instruction i : listing.getInstructions(text, true)) {
        if (!i.getFlowType().isCall()) continue;
        for (Address t : i.getFlows()) {
          if (text.contains(t) && listing.getInstructionAt(t) == null && listing.getDefinedDataAt(t) == null) targets.add(t);
        }
      }
      for (Address t : targets) {
        if (listing.getInstructionAt(t) == null && disassemble(t) && fm.getFunctionContaining(t) == null && mk(t) != null) made++;
      }
      p0 += made;
      if (made == 0) break;
    }
    for (Function f : fm.getFunctions(text, true)) {
      if (f.getBody().getNumAddresses() <= 1 && listing.getInstructionAt(f.getEntryPoint()) == null) {
        disassemble(f.getEntryPoint());
        ghidra.app.cmd.function.CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
        fixed++;
      }
    }
    println("pass 0: " + p0 + " functions at undisassembled call targets, " + fixed + " empty bodies fixed");

    // Pass 1: code pointers in data.
    int p1 = 0, p1raw = 0;
    for (MemoryBlock b : mem.getBlocks()) {
      if (!b.isInitialized() || b.isExecute() || b.getName().startsWith(".cms") || b.getName().equals(".text")) continue;
      Address a = b.getStart().getNewAddress((b.getStart().getOffset() + 3) & ~3L);
      int run = 0;
      List<Address> pending = new ArrayList<>();
      while (a.compareTo(b.getEnd()) < 0 && !monitor.isCancelled()) {
        long v;
        try { v = mem.getInt(a) & 0xffffffffL; } catch (MemoryAccessException e) { break; }
        Address t = null;
        try { t = toAddr(v); } catch (Exception e) { }
        boolean inText = t != null && text.contains(t);
        if (inText) {
          run++;
          if (orphan(t)) {
            if (mk(t) != null) p1++;
          } else if (listing.getInstructionAt(t) == null && listing.getDefinedDataAt(t) == null
                     && fm.getFunctionContaining(t) == null && (v & 15) == 0 && afterPaddingOrReturn(t)) {
            pending.add(t);
          }
        } else {
          if (run >= 2) {
            for (Address t2 : pending) {
              if (listing.getInstructionAt(t2) == null && disassemble(t2) && mk(t2) != null) p1raw++;
            }
          }
          run = 0;
          pending.clear();
        }
        a = a.add(4);
      }
    }
    // Code addresses taken by instructions (mov [x], offset LAB / push offset LAB), e.g. the
    // D3DX PSGP table fillers. Switch tables are defined data and are skipped.
    int p1ins = 0;
    ReferenceManager rm1 = currentProgram.getReferenceManager();
    List<Address> taken = new ArrayList<>();
    for (Address src : rm1.getReferenceSourceIterator(text, true)) {
      for (Reference r : rm1.getReferencesFrom(src)) {
        Address t = r.getToAddress();
        if (!r.getReferenceType().isData() || !text.contains(t) || fm.getFunctionAt(t) != null) continue;
        if (listing.getDefinedDataAt(t) != null) continue;
        taken.add(t);
      }
    }
    for (Address t : taken) {
      if (fm.getFunctionContaining(t) != null) continue;
      if (orphan(t)) { if (mk(t) != null) p1ins++; }
      else if (listing.getInstructionAt(t) == null && (t.getOffset() & 15) == 0 && afterPaddingOrReturn(t) && disassemble(t) && mk(t) != null) p1ins++;
    }
    println("pass 1: " + p1 + " functions at orphan code pointers, " + p1raw + " at new code pointers, " + p1ins + " at code addresses taken by instructions");

    // Pass 2: orphan runs. Never starts on int3 padding.
    int p2 = 0, round = 0;
    List<Function> made2 = new ArrayList<>();
    while (round++ < 6 && !monitor.isCancelled()) {
      int made = 0;
      Address runStart = null;
      List<Address> starts = new ArrayList<>();
      for (Instruction i : listing.getInstructions(text, true)) {
        Address a = i.getAddress();
        if (fm.getFunctionContaining(a) != null) { runStart = null; continue; }
        if (runStart == null) {
          runStart = a;
          if (getByte(a) != (byte) 0xCC && afterPaddingOrReturn(a)) starts.add(a);
        }
        if (!i.hasFallthrough()) runStart = null;
      }
      for (Address s : starts) {
        if (fm.getFunctionContaining(s) != null) continue;
        Function f = mk(s);
        if (f != null) { made++; made2.add(f); }
      }
      p2 += made;
      if (made == 0) break;
    }
    // A pass-2 function that jumps into the middle of another function is a fragment of
    // it (switch case, handler), not a function: remove it again and leave it orphan.
    int frags = 0;
    for (Function f : made2) {
      if (f.isDeleted()) continue;
      boolean frag = false;
      for (Instruction i : listing.getInstructions(f.getBody(), true)) {
        if (!i.getFlowType().isJump()) continue;
        for (Address t : i.getFlows()) {
          Function o = fm.getFunctionContaining(t);
          if (o != null && o != f && !o.getEntryPoint().equals(t)) frag = true;
        }
      }
      if (frag) { removeFunction(f); frags++; }
    }
    p2 -= frags;
    println("pass 2: " + p2 + " functions at orphan runs (" + round + " rounds), " + frags + " fragments removed");

    analyzeChanges(currentProgram);

    // Re-analysis recovers switch tables inside the new functions, which can show that
    // a function made above is really a case block or other piece of a neighbour.
    // Remove those and let the parent take the code back. A plain jmp from one other
    // function is not enough on its own (MSVC tail-calls real functions that way), so
    // that case also needs the function to have no call or data references.
    ReferenceManager rm = currentProgram.getReferenceManager();
    Set<Function> parents = new HashSet<>();
    int healed = 0;
    for (Address e : created) {
      Function f = fm.getFunctionAt(e);
      if (f == null) continue;
      boolean frag = f.getName().startsWith("caseD_") || f.getName().startsWith("switchD");
      Set<Function> jumpers = new HashSet<>();
      boolean otherRefs = false;
      for (Reference r : rm.getReferencesTo(e)) {
        RefType t = r.getReferenceType();
        Function o = fm.getFunctionContaining(r.getFromAddress());
        if (t.isJump() && o != null && o != f) {
          jumpers.add(o);
          if (t.isComputed() || t.isConditional()) frag = true;
        } else if (!t.isJump()) {
          otherRefs = true;
        }
      }
      if (!frag && !otherRefs && jumpers.size() == 1) frag = true;
      if (frag) { parents.addAll(jumpers); removeFunction(f); healed++; }
    }
    int fixedParents = 0;
    for (Function p : parents) {
      if (!p.isDeleted()) { ghidra.app.cmd.function.CreateFunctionCmd.fixupFunctionBody(currentProgram, p, monitor); fixedParents++; }
    }
    println("cleanup: " + healed + " case/fragment functions removed, " + fixedParents + " parent bodies rebuilt");
    String after = counts(watch);
    println("after: " + after);
    if (report != null) {
      try (PrintWriter pw = new PrintWriter(report)) {
        pw.println("before: " + before);
        pw.println("pass 1: " + p1 + " at orphan code pointers, " + p1raw + " at new code pointers, " + p1ins + " at taken code addresses");
        pw.println("pass 2: " + p2 + " at orphan runs");
        pw.println("after:  " + after);
      }
    }
  }
}
