"""Check executable provenance and release eligibility in a temporary Git repository."""

import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def run(*args, **kwargs):
    return subprocess.check_output(args, text=True, timeout=10, **kwargs)


def read_info(generated):
    source = generated.read_text(encoding="utf-8")
    literal = re.search(r'^constexpr std::string_view report = ("(?:[^"\\]|\\.)*");$', source, flags=re.M)
    report = json.loads(literal[1]).encode("utf-8")
    fields = re.findall(r"^    \.(\w+) = report\.subview\((\d+), (\d+)\),$", source, flags=re.M)
    return {name: report[int(offset):int(offset) + int(length)].decode("utf-8")
            for name, offset, length in fields}


def check_executable(server, generated):
    info = read_info(generated)
    title, *lines = run(server, "--config", "/nonexistent/updater.toml", "--version").splitlines()
    fields = dict(line.split(": ", 1) for line in lines)
    assert title == f"Shared Updater Server {info['version']}"
    assert fields["Status"] == info["status"]
    assert fields["Build"] == f"{info['configuration']} ({info['variant']})"
    assert fields["Build date"] == f"{info['date']} UTC"
    assert fields["Git commit"] == f"{info['git_commit']} ({info['git_tree']})"
    assert fields["Git tag"] == info["git_tag"]


def check_release_status(root, cmake, generator, git_path):
    repo = root / "repo"
    repo.mkdir()
    generated = root / "build_info.cpp"
    env = dict(os.environ, SOURCE_DATE_EPOCH="1704067200")

    def git(*args):
        return run(git_path, "-C", repo, "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                   "-c", "commit.gpgSign=false", "-c", "tag.gpgSign=false", "-c", "core.hooksPath=/dev/null", *args)

    def generate(config="RelWithDebInfo", variant="normal", sanitizer="none", source=repo):
        run(cmake, f"-DSOURCE_DIR={source}", f"-DGIT_EXECUTABLE={git_path}", "-DPROJECT_VERSION=0.1.0",
            f"-DBUILD_CONFIG={config}", f"-DBUILD_VARIANT={variant}", f"-DBUILD_SANITIZER={sanitizer}",
            f"-DOUTPUT_FILE={generated}", "-P", generator, env=env)
        return read_info(generated)

    git("init", "-q", "-b", "main")
    (repo / ".gitignore").write_text("/ignored\n")
    source = repo / "source.txt"
    source.write_text("initial\n")
    git("add", ".gitignore", "source.txt")
    git("commit", "-qm", "initial")
    commit = git("rev-parse", "HEAD").strip()

    initial = generate()
    assert initial == dict(version="0.1.0", status="development", configuration="RelWithDebInfo",
                           variant="normal", date="2024-01-01", git_commit=commit, git_tree="clean", git_tag="-")
    tag = 'keep/"Długosz"'
    git("tag", "-a", tag, "-m", "Keep this build")
    (repo / "ignored").touch()
    release = generate()
    assert release == dict(initial, version=tag, git_tag=tag, status="release")
    mtime = generated.stat().st_mtime_ns
    assert generate() == release and generated.stat().st_mtime_ns == mtime

    untracked = repo / "untracked.txt"
    untracked.touch()
    assert generate() == dict(release, status="development", git_tree="dirty")
    untracked.unlink()
    source.write_text("changed\n")
    assert generate()["status"] == "development"
    git("add", "source.txt")
    assert generate()["git_tree"] == "dirty"
    git("restore", "--staged", "--worktree", "--", "source.txt")
    assert generate(config="Debug")["status"] == "development"
    assert generate(variant="profile") == dict(release, status="development", variant="profile")
    assert generate(sanitizer="address") == dict(release, status="development", variant="sanitizer-address")

    git("commit", "-qm", "past the tag", "--allow-empty")
    next_commit = git("rev-parse", "HEAD").strip()
    assert generate() == dict(initial, git_commit=next_commit)
    assert generate(source=root) == dict(initial, git_commit="unknown", git_tree="unknown")


if __name__ == "__main__":
    server, generated, cmake, generator, git_path = sys.argv[1:]
    check_executable(server, Path(generated))
    with tempfile.TemporaryDirectory(prefix="updater-version-") as directory:
        check_release_status(Path(directory), cmake, generator, git_path)
