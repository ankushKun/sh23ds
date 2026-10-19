// Dumps everything people have added to the saved Ghidra project as text, so it can live in git
// without any program bytes: function boundaries, names, tags and typed signatures, hand-made labels
// and the data types applied at them, comments and bookmarks. ImportProject.java puts it back into
// a fresh import of the user's own exe (tools/ghidra/rebuild.sh).
//
// Files (tab separated, sorted by address; tabs, newlines and backslashes in text are escaped):
//   functions.tsv  entry, name (empty for FUN_*), name source, body ranges, thunk target,
//                  calling convention, signature source, signature (user-defined ones only), tags
//   symbols.tsv    labels that are not functions: address, name, namespace, source, primary
//   data.tsv       the data type applied at each user label (for typed globals)
//   comments.tsv   address, kind (EOL PRE POST PLATE REPEATABLE), text
//   bookmarks.tsv  address, type, category, comment (not the analyzers' Analysis/Error ones)
// Args: <outdir>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class ExportProject extends GhidraScript {
  static final String[] KINDS = { "EOL", "PRE", "POST", "PLATE", "REPEATABLE" };

  static String esc(String s) {
    return s == null ? "" : s.replace("\\", "\\\\").replace("\t", "\\t").replace("\r", "").replace("\n", "\\n");
  }

  static String body(AddressSetView b) {
    StringBuilder sb = new StringBuilder();
    for (AddressRange r : b) {
      if (sb.length() > 0) sb.append(',');
      sb.append("0x").append(r.getMinAddress()).append("-0x").append(r.getMaxAddress());
    }
    return sb.toString();
  }

  static boolean kept(SourceType s) {
    return s == SourceType.USER_DEFINED || s == SourceType.IMPORTED;
  }

  public void run() throws Exception {
    File out = new File(getScriptArgs()[0]);
    out.mkdirs();
    FunctionManager fm = currentProgram.getFunctionManager();
    SymbolTable st = currentProgram.getSymbolTable();
    Listing listing = currentProgram.getListing();
    int nf = 0, ns = 0, nd = 0, nc = 0, nb = 0;

    try (PrintWriter pw = new PrintWriter(new File(out, "functions.tsv"), "UTF-8")) {
      pw.println("entry\tname\tname_source\tbody\tthunk\tcallconv\tsig_source\tsignature\ttags");
      for (Function f : fm.getFunctions(true)) {
        Symbol s = f.getSymbol();
        String name = s.getSource() == SourceType.DEFAULT ? "" : f.getName(true);
        String thunk = "";
        if (f.isThunk() && f.getThunkedFunction(false) != null) {
          Function t = f.getThunkedFunction(false);
          thunk = t.isExternal() ? "ext:" + t.getParentNamespace().getName() + "::" + t.getName() : "0x" + t.getEntryPoint();
        }
        String sig = f.getSignatureSource() == SourceType.USER_DEFINED ? f.getPrototypeString(true, false) : "";
        TreeSet<String> tags = new TreeSet<>();
        for (FunctionTag t : f.getTags()) tags.add(t.getName());
        pw.println("0x" + f.getEntryPoint() + "\t" + esc(name) + "\t" + s.getSource() + "\t" + body(f.getBody()) + "\t" + esc(thunk)
          + "\t" + esc(f.getCallingConventionName()) + "\t" + f.getSignatureSource() + "\t" + esc(sig) + "\t" + esc(String.join(",", tags)));
        nf++;
      }
    }

    try (PrintWriter ps = new PrintWriter(new File(out, "symbols.tsv"), "UTF-8");
         PrintWriter pd = new PrintWriter(new File(out, "data.tsv"), "UTF-8")) {
      ps.println("addr\tname\tnamespace\tsource\tprimary");
      pd.println("addr\tname\tdatatype");
      for (Symbol s : st.getAllSymbols(true)) {
        if (s.isExternal() || !kept(s.getSource())) continue;
        if (s.getSymbolType() != SymbolType.LABEL) continue;
        String nsName = s.getParentNamespace().isGlobal() ? "" : s.getParentNamespace().getName(true);
        ps.println("0x" + s.getAddress() + "\t" + esc(s.getName()) + "\t" + esc(nsName) + "\t" + s.getSource() + "\t" + s.isPrimary());
        ns++;
        if (s.getSource() != SourceType.USER_DEFINED) continue;
        Data d = listing.getDataAt(s.getAddress());
        if (d != null && d.isDefined() && !(d.getDataType() instanceof Undefined)) {
          pd.println("0x" + s.getAddress() + "\t" + esc(s.getName()) + "\t" + esc(d.getDataType().getDataTypePath().getPath()));
          nd++;
        }
      }
    }

    try (PrintWriter pw = new PrintWriter(new File(out, "comments.tsv"), "UTF-8")) {
      pw.println("addr\tkind\ttext");
      TreeMap<Address, List<String>> rows = new TreeMap<>();
      for (int k = 0; k < KINDS.length; k++) {
        AddressIterator it = listing.getCommentAddressIterator(k, currentProgram.getMemory(), true);
        while (it.hasNext()) {
          Address a = it.next();
          rows.computeIfAbsent(a, x -> new ArrayList<>()).add(KINDS[k] + "\t" + esc(listing.getComment(k, a)));
        }
      }
      for (var e : rows.entrySet()) for (String r : e.getValue()) { pw.println("0x" + e.getKey() + "\t" + r); nc++; }
    }

    try (PrintWriter pw = new PrintWriter(new File(out, "bookmarks.tsv"), "UTF-8")) {
      pw.println("addr\ttype\tcategory\tcomment");
      BookmarkManager bm = currentProgram.getBookmarkManager();
      for (BookmarkType t : bm.getBookmarkTypes()) {
        String ty = t.getTypeString();
        if (ty.equals(BookmarkType.ANALYSIS) || ty.equals(BookmarkType.ERROR)) continue;
        Iterator<Bookmark> it = bm.getBookmarksIterator(ty);
        while (it.hasNext()) {
          Bookmark b = it.next();
          pw.println("0x" + b.getAddress() + "\t" + ty + "\t" + esc(b.getCategory()) + "\t" + esc(b.getComment()));
          nb++;
        }
      }
    }
    println("exported " + nf + " functions, " + ns + " labels, " + nd + " typed globals, " + nc + " comments, " + nb + " bookmarks");
  }
}
