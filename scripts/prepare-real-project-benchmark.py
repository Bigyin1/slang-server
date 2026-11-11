#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["pyyaml", "gitpython"]
# ///
# SPDX-FileCopyrightText: Hudson River Trading
# SPDX-License-Identifier: MIT
"""Prepare a server benchmark from an RTLMeter checkout without changing its RTL."""

import argparse
import importlib.util
import json
from pathlib import Path
from types import ModuleType

from git import Repo


def benchmark_tools() -> ModuleType:
    """Reuse slang's descriptor merging and command-line construction."""
    script = (
        Path(__file__).resolve().parents[1]
        / "external/slang/tests/benchmarks/run_benchmarks.py"
    )
    spec = importlib.util.spec_from_file_location("slang_benchmarks", script)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Cannot load benchmark tools from {script}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def compile_arguments(rtlmeter: Path, design: str) -> tuple[list[str], list[str]]:
    tools = benchmark_tools()
    directory = rtlmeter / "designs" / design
    descriptor = tools.load_descriptor(directory)
    if descriptor.get("configurations"):
        raise ValueError(
            "Select a design with a default configuration for this benchmark"
        )
    extra = tools.load_extra_args(
        Path(__file__).resolve().parents[1]
        / "external/slang/tests/benchmarks/slang_args.yaml"
    )
    extra_args = tools.resolve_extra_args(
        tools.extra_args_for(design, "default", extra), directory
    )
    command = tools.build_slang_command(
        Path("slang"), directory, descriptor["compile"], extra_args
    )
    if command[:4] != ["slang", "--quiet", "--time-stats", "-"]:
        raise ValueError("The slang benchmark command prefix has changed")
    arguments = command[4:]
    if "--single-unit" not in arguments:
        arguments.insert(0, "--single-unit")
    sources = [str(rtlmeter / "rtl/__rtlmeter_utils.sv")]
    sources.extend(arg for arg in extra_args if not arg.startswith("-"))
    sources.extend(
        str(directory / name) for name in descriptor["compile"]["verilogSourceFiles"]
    )
    return arguments, list(dict.fromkeys(sources))


def write_workspace(
    workspace: Path, arguments: list[str], index_dirs: list[Path]
) -> Path:
    """Write the build command file and the server's index configuration."""
    config = workspace / ".slang/server.json"
    config.parent.mkdir(parents=True, exist_ok=True)
    build = workspace / "design.f"
    build.write_text(
        "\n".join(json.dumps(arg, ensure_ascii=False) for arg in arguments) + "\n",
        encoding="utf-8",
    )
    config.write_text(
        json.dumps(
            {"index": [{"dirs": [str(directory) for directory in index_dirs]}]},
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    return build


def prepare(args: argparse.Namespace) -> Path:
    root = args.rtlmeter.resolve(strict=True)
    revision = Repo(root).head.commit.hexsha
    workspace = args.workspace.resolve()
    config = workspace / ".slang/server.json"
    if config.exists():
        raise FileExistsError(f"Refusing to replace existing server config: {config}")
    arguments, sources = compile_arguments(root, args.design)
    edited = [
        str((root / "designs" / args.design / path).resolve()) for path in args.edit
    ]
    if not edited or not set(edited).issubset(sources):
        raise ValueError("Each --edit must name a top-level source from the descriptor")
    for source in sources:
        if not Path(source).is_file():
            raise FileNotFoundError(source)
    build = write_workspace(
        workspace, arguments, [root / "designs" / args.design / "src", root / "rtl"]
    )
    manifest = {
        "name": args.design,
        "revision": revision,
        "workspace": str(workspace),
        "buildFile": str(build),
        "sourceFiles": sources,
        "editFiles": edited,
        "cycles": args.cycles,
        "editsPerCycle": args.edits,
    }
    output = workspace / "benchmark.json"
    output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rtlmeter", type=Path)
    parser.add_argument("workspace", type=Path)
    parser.add_argument("--design", default="OpenTitan")
    parser.add_argument(
        "--edit", action="append", required=True, help="Path relative to designs/DESIGN"
    )
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--edits", type=int, default=3)
    args = parser.parse_args()
    if args.cycles < 1 or args.edits < 1:
        parser.error("--cycles and --edits must be positive")
    print(prepare(args))


if __name__ == "__main__":
    main()
