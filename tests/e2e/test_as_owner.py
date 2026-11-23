""".devcontainer/as-owner.sh, compose.yaml's entrypoint: it runs the command, hands its exit
status, stdin and signals through, and gives what it created in the checkout to the checkout's
owner. Runs as root, as in the container (docker compose run --rm build tests/run.sh)."""
import os
import signal
import subprocess
import time
import unittest

from tests import util

OWNER = os.path.join(util.ROOT, ".devcontainer", "as-owner.sh")


@unittest.skipUnless(hasattr(os, "geteuid") and os.geteuid() == 0, "needs root (as in the container)")
class AsOwner(util.TestCase):
    def setUp(self):
        super().setUp()
        self.co = self.path("checkout")
        os.makedirs(f"{self.co}/work")
        os.chown(self.co, 1234, 1234)

    def owner(self, *cmd, **kw):
        return subprocess.run(["bash", OWNER, *cmd], cwd=self.co, capture_output=True, text=True, timeout=60, **kw)

    def test_hands_back_what_it_made(self):
        proc = self.owner("bash", "-c", "mkdir -p out/d; touch out/a out/d/b work/c; ln -s a out/l; exit 7")
        self.assertEqual(proc.returncode, 7, proc.stderr)
        for rel in ("out", "out/a", "out/d", "out/d/b"):
            self.assertEqual(os.stat(f"{self.co}/{rel}").st_uid, 1234, rel)
        self.assertEqual(os.lstat(f"{self.co}/out/l").st_uid, 1234)
        # work/ is the Docker volume: left to root.
        self.assertEqual(os.stat(f"{self.co}/work/c").st_uid, 0)

    def test_stdin_reaches_the_command(self):
        self.assertEqual(self.owner("cat", input="hi\n").stdout, "hi\n")

    def test_root_owned_checkout_is_left_alone(self):
        os.chown(self.co, 0, 0)
        self.assertEqual(self.owner("touch", "x").returncode, 0)
        self.assertEqual(os.stat(f"{self.co}/x").st_uid, 0)

    def test_passes_signals_on(self):
        for sig, code in ((signal.SIGTERM, 5), (signal.SIGINT, 6)):
            script = f"trap 'echo got; touch out_sig; exit {code}' {sig.name[3:]}; echo ready; sleep 30 & wait"
            # Started from Python, SIGINT isn't ignored, as for PID 1 in the container.
            proc = subprocess.Popen(["bash", OWNER, "bash", "-c", script], cwd=self.co, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, text=True)
            self.assertEqual(proc.stdout.readline(), "ready\n")
            time.sleep(0.2)
            proc.send_signal(sig)
            out, _ = proc.communicate(timeout=30)
            self.assertEqual((proc.returncode, out), (code, "got\n"), sig.name)
            self.assertEqual(os.stat(f"{self.co}/out_sig").st_uid, 1234)
            os.remove(f"{self.co}/out_sig")
