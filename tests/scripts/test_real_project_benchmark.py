# SPDX-FileCopyrightText: Hudson River Trading
# SPDX-License-Identifier: MIT
"""RTLMeter benchmark preparation: paths, flags, and rejected inputs."""

import argparse
import importlib.util
import json
from pathlib import Path
from types import ModuleType

from git import Actor, Repo
import pytest
import yaml


@pytest.fixture
def preparer() -> ModuleType:
    script = (
        Path(__file__).resolve().parents[2]
        / "scripts/prepare-real-project-benchmark.py"
    )
    spec = importlib.util.spec_from_file_location("prepare_benchmark", script)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def corpus(tmp_path: Path) -> Path:
    root = tmp_path / "corpus with spaces"
    source = root / "designs/Example/src"
    source.mkdir(parents=True)
    (root / "rtl").mkdir()
    (root / "rtl/__rtlmeter_utils.sv").write_text("module utils; endmodule\n")
    (source / "top.sv").write_text("module top; endmodule\n")
    (source / "defs.svh").write_text("`define VALUE 1\n")
    descriptor = {
        "compile": {
            "verilogSourceFiles": ["src/top.sv"],
            "verilogIncludeFiles": ["src/defs.svh"],
            "verilogDefines": {"VALUE": 1},
            "topModule": "top",
            "mainClock": "top.clk",
        }
    }
    (source.parent / "descriptor.yaml").write_text(yaml.safe_dump(descriptor))
    repo = Repo.init(root, initial_branch="test-fixture")
    repo.index.add(["designs/Example/descriptor.yaml"])
    actor = Actor("Benchmark fixture", "fixture@example.invalid")
    repo.index.commit("Benchmark fixture", author=actor, committer=actor)
    return root


def arguments(
    corpus: Path, workspace: Path, edited: str = "src/top.sv"
) -> argparse.Namespace:
    return argparse.Namespace(
        rtlmeter=corpus,
        workspace=workspace,
        design="Example",
        edit=[edited],
        cycles=3,
        edits=3,
    )


def test_prepares_sources_flags_and_revision(
    preparer: ModuleType, corpus: Path, tmp_path: Path
) -> None:
    output = preparer.prepare(arguments(corpus, tmp_path / "workspace"))
    manifest = json.loads(output.read_text())
    tokens = [
        json.loads(line)
        for line in Path(manifest["buildFile"]).read_text().splitlines()
    ]
    assert "--single-unit" in tokens
    assert "-DVALUE=1" in tokens
    assert tokens[tokens.index("--top") + 1] == "top"
    assert str(corpus / "designs/Example/src/top.sv") in tokens
    assert manifest["revision"] == Repo(corpus).head.commit.hexsha
    assert manifest["sourceFiles"] == [
        str(corpus / "rtl/__rtlmeter_utils.sv"),
        str(corpus / "designs/Example/src/top.sv"),
    ]
    assert (
        corpus / "designs/Example/src/top.sv"
    ).read_text() == "module top; endmodule\n"


def test_rejects_edit_outside_build(
    preparer: ModuleType, corpus: Path, tmp_path: Path
) -> None:
    workspace = tmp_path / "workspace"
    with pytest.raises(ValueError, match="top-level source"):
        preparer.prepare(arguments(corpus, workspace, "src/defs.svh"))
    assert not workspace.exists()


def test_preserves_existing_configuration(
    preparer: ModuleType, corpus: Path, tmp_path: Path
) -> None:
    workspace = tmp_path / "workspace"
    config = workspace / ".slang/server.json"
    config.parent.mkdir(parents=True)
    config.write_text("existing configuration\n")
    with pytest.raises(FileExistsError):
        preparer.prepare(arguments(corpus, workspace))
    assert config.read_text() == "existing configuration\n"


def test_rejects_missing_source(
    preparer: ModuleType, corpus: Path, tmp_path: Path
) -> None:
    (corpus / "designs/Example/src/top.sv").unlink()
    workspace = tmp_path / "workspace"
    with pytest.raises(FileNotFoundError):
        preparer.prepare(arguments(corpus, workspace))
    assert not workspace.exists()
