"""Where the converters reach outside Python: ffmpeg and ffprobe, large file copies into the pack,
the texture encoder and running work side by side.

On a computer these are subprocesses, shutil copies, tex3ds and a thread pool, as before. In the
browser packer (Pyodide, sys.platform == 'emscripten'; site/packer/) there are no processes or
threads, so the page provides the same operations as the JavaScript object `sh2host`:

  sh2host.run(argv, input)   runs ffmpeg / ffprobe (ffmpeg.wasm) on files in Python's file system;
                             returns {code, stdout: Uint8Array, stderr: string}
  sh2host.copy(src, dst)     adds the game file src to the pack as dst without reading it into
                             Python (the music is 740 MB of plain copies)
  sh2host.texture(...)       see t3x.py
  sh2host.progress(f, ...)   how far the current step has got, 0..1, for its progress bar, with a
                             count of what it is working through (120 / 486 textures)

progress() and part() report that progress: a step is split into parts (pack.py), a converter splits
its part further (an area, a movie, a scene), and progress(f) says how far the innermost part is. On a
computer they do nothing.
"""
import contextlib
import os
import shutil
import subprocess
import sys

BROWSER = sys.platform == 'emscripten'
_span = [0.0, 1.0]     # the share of the current step that the innermost part() covers
_last = [-1.0, None]    # the last progress and count sent


def reset():
    """A new step: progress starts again at 0."""
    _span[:] = [0.0, 1.0]
    _last[:] = [-1.0, None]


def progress(f, done=None, total=None, what=None):
    """The innermost part is a fraction f (0..1) done; done / total counts what it is working
    through when it knows ('textures in ap', 'movies')."""
    if not BROWSER:
        return
    v = _span[0] + max(0.0, min(1.0, f)) * (_span[1] - _span[0])
    if abs(v - _last[0]) >= 0.002 or (v >= 1.0 > _last[0]) or (done is not None and (done, total) != _last[1]):
        _last[0] = v
        if done is not None:
            _last[1] = (done, total)
        from js import sh2host
        sh2host.progress(v, done, total, what)


@contextlib.contextmanager
def part(lo, hi):
    """Work that covers fractions lo..hi of the current part; progress() inside it reports within it."""
    old = _span[:]
    _span[:] = [old[0] + lo * (old[1] - old[0]), old[0] + hi * (old[1] - old[0])]
    try:
        yield
    finally:
        _span[:] = old
        progress(hi)


def parts(weights):
    """(lo, hi) for consecutive pieces of work with these relative weights."""
    total = float(sum(weights)) or 1.0
    out, at = [], 0.0
    for w in weights:
        out.append((at / total, (at + w) / total))
        at += w
    return out


def run(argv, input=None, capture=False):
    """Runs a tool (ffmpeg, ffprobe) and returns its standard output as bytes when capture is set.
    Raises subprocess.CalledProcessError when it fails, as subprocess.run(check=True) does."""
    if not BROWSER:
        r = subprocess.run(argv, input=input, capture_output=capture, check=True)
        return r.stdout if capture else None
    from js import sh2host
    from pyodide.ffi import to_js
    r = sh2host.run(to_js(list(argv)), to_js(input) if input is not None else None, _span[0], _span[1])
    if r.code != 0:
        raise subprocess.CalledProcessError(r.code, list(argv), stderr=str(r.stderr))
    return r.stdout.to_bytes() if capture else None


def copy(src, dst):
    """Copies src to dst (shutil.copy2). In the browser the page streams the game file into the
    output itself, so the file never passes through Python's memory."""
    if not BROWSER:
        shutil.copy2(src, dst)
        return
    from js import sh2host
    sh2host.copy(os.path.abspath(src), os.path.abspath(dst))


def pmap(fn, items, jobs):
    """map(fn, items) over up to jobs threads; one after another in the browser, which has none."""
    items = list(items)
    if BROWSER or jobs <= 1 or len(items) <= 1:
        return [fn(x) for x in items]
    import concurrent.futures
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        return list(ex.map(fn, items))
