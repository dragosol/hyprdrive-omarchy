#!/usr/bin/env python3
"""ToolSearch.cpp's dictionary and translation scripts, run as the overview
runs them (sh -c SCRIPT name ARGS, the user's text on stdin) against stand-in
curl and jq that log every argument list: the text must never be in one
(marketplace review, #11392: /proc/<pid>/cmdline is readable by every local
account). The stand-in curl answers like the real services; jq is the real one."""
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = (ROOT / "ToolSearch.cpp").read_text()
failed = checks = 0


def expect(ok, what):
    global failed, checks
    checks += 1
    if not ok:
        failed += 1
        print("FAIL", what)


def script(name):
    m = re.search(r'constexpr const char\* %s =\s*R"(SH|JQ)\((.*?)\)\1";' % name, SRC, re.S)
    assert m, name
    return m.group(2)


SECRET = "SECRET-7f3a my bank PIN is 4471 $(touch CANARY) `id` \"q\" it's"
REAL_JQ = shutil.which("jq")
tmp = pathlib.Path(tempfile.mkdtemp(prefix="hd-tool-scripts-"))
bin_ = tmp / "bin"
bin_.mkdir()
log = tmp / "argv.log"
seen_stdin = tmp / "curl-stdin.log"

# curl: log its argv, keep what it reads on stdin, answer like the service.
(bin_ / "curl").write_text(r'''#!/bin/sh
printf '%s\n' "curl $*" >> "$HD_LOG"
cat > "$HD_STDIN.tmp"; cat "$HD_STDIN.tmp" >> "$HD_STDIN"
case "$*" in
  *wiktionary*|*" -K -"*) printf '%s' '{"en":[{"partOfSpeech":"Noun","definitions":[{"definition":"a <b>word</b>","examples":["an example"]}]}]}' ;;
  *mymemory*) printf '%s' '{"responseStatus":200,"responseData":{"translatedText":"Bonjour","detectedLanguage":"en"},"matches":[]}' ;;
  *) printf '%s' '{"translatedText":"Bonjour","detectedLanguage":{"language":"en"}}' ;;
esac
''')
# jq: the real one, its argv logged.
(bin_ / "jq").write_text('#!/bin/sh\nprintf \'%%s\\n\' "jq $*" >> "$HD_LOG"\nexec %s "$@"\n' % REAL_JQ)
for f in ("curl", "jq"):
    (bin_ / f).chmod(0o755)

env = dict(os.environ, PATH="%s:/usr/bin:/bin" % bin_, HD_LOG=str(log), HD_STDIN=str(seen_stdin))


def run(name, args, stdin):
    for f in (log, seen_stdin):
        f.write_text("")
    p = subprocess.run(["sh", "-c", script(name), "hyprdrive-tool", *args], input=stdin.encode(), capture_output=True, env=env, cwd=tmp, timeout=30)
    return p.returncode, p.stdout.decode(), log.read_text(), seen_stdin.read_text()


# Translation, MyMemory: the text reaches curl on stdin (q@-), never in argv.
rc, out, argv, stdin = run("MYMEMORY_SCRIPT", ["autodetect", "fr"], SECRET)
expect(rc == 0 and out.startswith("200\ten\tBonjour"), "MyMemory: parsed the answer (%r)" % out)
expect("SECRET" not in argv and "4471" not in argv, "MyMemory: the text is in no argument list:\n" + argv)
expect(stdin == SECRET, "MyMemory: curl read the text, byte for byte, from stdin")

# Translation, self-hosted LibreTranslate: the JSON body reaches curl on stdin.
rc, out, argv, stdin = run("LIBRE_SCRIPT", ["auto", "fr", "http://127.0.0.1:5000"], SECRET)
expect(rc == 0 and out.startswith("200\ten\tBonjour"), "LibreTranslate: parsed the answer (%r)" % out)
expect("SECRET" not in argv and "4471" not in argv, "LibreTranslate: the text is in no argument list:\n" + argv)
try:
    body = json.loads(stdin or "{}")
except ValueError:
    body = {}
expect(body.get("q") == SECRET, "LibreTranslate: curl read the body, with the text exact, from stdin")

# The dictionary: the word reaches curl in a config on stdin.
rc, out, argv, stdin = run("DEFINE_SCRIPT", [script("DEFINE_JQ"), "hyprdrive-test"], "SECRETWORD\nsecretword\n")
expect(rc == 0 and out.startswith("Noun\ta word"), "dictionary: parsed the answer (%r)" % out)
expect("SECRET" not in argv.upper(), "dictionary: the word is in no argument list:\n" + argv)
expect('url = "https://en.wiktionary.org/api/rest_v1/page/definition/SECRETWORD"' in stdin, "dictionary: curl read its URL from stdin")

expect(not (tmp / "CANARY").exists(), "nothing in the text ran")
shutil.rmtree(tmp, ignore_errors=True)
print("tool_scripts_test: %d checks, %d failed" % (checks, failed))
sys.exit(1 if failed else 0)
