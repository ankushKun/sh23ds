// Dumps what tools/module_map.py needs to map functions to modules.
//   functions.tsv  entry, name, body size, min/max address of the body, thunk flag
//   strings.tsv    function, string address, string text (strings referenced by the body,
//                  directly or through one pointer, e.g. a table of message pointers)
//   calls.tsv      caller, callee (direct calls and call overrides, thunks resolved)
//   globals.tsv    function, address (data refs into .data/.rdata that are not strings)
//   codeptrs.tsv   function, function whose address it takes (mov/push offset FUN, not calls)
// Args: <outdir>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class ExportGraph extends GhidraScript {
  String clean(String s) {
    return s.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n").replace("\r", "\\r");
  }

  String stringAt(Address a) {
    Data d = getDataAt(a);
    if (d == null || !d.hasStringValue()) return null;
    StringDataInstance s = StringDataInstance.getStringDataInstance(d);
    String v = s.getStringValue();
    return v == null ? null : clean(v);
  }

  public void run() throws Exception {
    String out = getScriptArgs()[0];
    new File(out).mkdirs();
    FunctionManager fm = currentProgram.getFunctionManager();
    ReferenceManager rm = currentProgram.getReferenceManager();
    try (PrintWriter pf = new PrintWriter(out + "/functions.tsv");
         PrintWriter ps = new PrintWriter(out + "/strings.tsv");
         PrintWriter pc = new PrintWriter(out + "/calls.tsv");
         PrintWriter pg = new PrintWriter(out + "/globals.tsv");
         PrintWriter pp = new PrintWriter(out + "/codeptrs.tsv")) {
      pf.println("entry\tname\tsize\tmin\tmax\tthunk");
      ps.println("function\taddr\ttext");
      pc.println("caller\tcallee");
      pg.println("function\taddr");
      pp.println("function\ttarget");
      for (Function f : fm.getFunctions(true)) {
        if (monitor.isCancelled()) break;
        AddressSetView body = f.getBody();
        pf.println(f.getEntryPoint() + "\t" + f.getName() + "\t" + body.getNumAddresses() + "\t" + body.getMinAddress() + "\t" + body.getMaxAddress() + "\t" + f.isThunk());
        Set<String> seenS = new HashSet<>(), seenC = new HashSet<>(), seenG = new HashSet<>(), seenP = new HashSet<>();
        for (Address src : rm.getReferenceSourceIterator(body, true)) {
          for (Reference r : rm.getReferencesFrom(src)) {
            Address to = r.getToAddress();
            RefType t = r.getReferenceType();
            if (t.isCall() || t == RefType.CALL_OVERRIDE_UNCONDITIONAL) {
              Function c = fm.getFunctionAt(to);
              if (c != null && c.isThunk() && c.getThunkedFunction(true) != null) c = c.getThunkedFunction(true);
              String k = c == null ? to.toString() : (c.isExternal() ? c.getName(true) : c.getEntryPoint().toString());
              if (seenC.add(k)) pc.println(f.getEntryPoint() + "\t" + k);
              continue;
            }
            if (!to.isMemoryAddress() || !t.isData()) continue;
            MemoryBlock b = getMemoryBlock(to);
            if (b == null) continue;
            if (b.isExecute()) {
              Function c = fm.getFunctionAt(to);
              if (c != null && c != f && seenP.add(to.toString())) pp.println(f.getEntryPoint() + "\t" + to);
              continue;
            }
            String s = stringAt(to);
            if (s == null) {
              // one level of indirection: pointer to a string
              Data d = getDataAt(to);
              if (d != null && d.isPointer() && d.getValue() instanceof Address) s = stringAt((Address) d.getValue());
            }
            if (s != null) { if (seenS.add(to.toString())) ps.println(f.getEntryPoint() + "\t" + to + "\t" + s); }
            else if (seenG.add(to.toString())) pg.println(f.getEntryPoint() + "\t" + to);
          }
        }
      }
    }
    println("exported graph to " + out);
  }
}
