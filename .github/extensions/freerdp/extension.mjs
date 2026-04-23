import { joinSession } from "@github/copilot-sdk/extension";
import { execFile as execFileCb } from "node:child_process";
import { join } from "node:path";
import { promisify } from "node:util";
import { access } from "node:fs/promises";

const execFile = promisify(execFileCb);
const REPO_ROOT = process.cwd();
let session;

function run(cmd, args, opts = {}) {
  return execFile(cmd, args, {
    cwd: REPO_ROOT,
    maxBuffer: 10 * 1024 * 1024,
    ...opts,
  });
}

// ── Tool 1: freerdp_sync ────────────────────────────────────────────────────

async function syncFork() {
  session.log("Fetching from origin…");
  await run("git", ["fetch", "origin"]);
  session.log("Fast-forwarding fork master…");
  await run("git", ["push", "ghpcb", "origin/master:master"]);
  return "Fork master fast-forwarded to origin/master.";
}

async function syncRebase() {
  const { stdout: status } = await run("git", ["status", "--porcelain"]);
  if (status.trim().length > 0) {
    return {
      textResultForLlm:
        "Working tree is dirty. Please commit or stash your changes before rebasing.\n\n" +
        "Dirty files:\n" +
        status,
      resultType: "error",
    };
  }

  session.log("Fetching from origin…");
  await run("git", ["fetch", "origin"]);

  session.log("Rebasing onto origin/master…");
  try {
    await run("git", ["rebase", "origin/master"]);
  } catch (err) {
    session.log("Rebase conflict detected — aborting rebase.");
    await run("git", ["rebase", "--abort"]);
    return {
      textResultForLlm:
        "Rebase failed due to conflicts. The rebase has been aborted and your branch is unchanged.\n\n" +
        "Conflict details:\n" +
        (err.stderr || err.stdout || err.message) +
        "\n\nResolve the conflicts manually with `git rebase origin/master`, fix each conflict, then `git rebase --continue`.",
      resultType: "error",
    };
  }

  const { stdout: head } = await run("git", ["rev-parse", "--short", "HEAD"]);
  return `Rebase complete. HEAD is now at ${head.trim()}.`;
}

const freerdpSync = {
  name: "freerdp_sync",
  description:
    "Keep the fork and working branch in sync with upstream. Can fast-forward the fork's master and/or rebase the current branch onto origin/master.",
  parameters: {
    type: "object",
    properties: {
      action: {
        type: "string",
        enum: ["fork", "rebase", "both"],
        default: "both",
        description:
          '"fork" updates the fork master, "rebase" rebases the current branch, "both" does fork then rebase.',
      },
    },
  },
  handler: async (params) => {
    const action = params.action || "both";
    const results = [];

    if (action === "fork" || action === "both") {
      const res = await syncFork();
      if (typeof res === "object") return res;
      results.push(res);
    }

    if (action === "rebase" || action === "both") {
      const res = await syncRebase();
      if (typeof res === "object") return res;
      results.push(res);
    }

    return results.join("\n");
  },
};

// ── Tool 2: freerdp_build ───────────────────────────────────────────────────

const PRESET_MAP = {
  sdl: "ci/cmake-preloads/config-client-sdl.cmake",
  x11: "ci/cmake-preloads/config-client-x11.cmake",
  wayland: "ci/cmake-preloads/config-client-wayland.cmake",
  macos: "ci/cmake-preloads/config-client-mac.cmake",
  windows: "ci/cmake-preloads/config-client-windows.cmake",
};

const freerdpBuild = {
  name: "freerdp_build",
  description:
    "Build a FreeRDP client for a given target platform. Supports dev (cmake+ninja) and full (platform bundle scripts) modes.",
  parameters: {
    type: "object",
    properties: {
      target: {
        type: "string",
        enum: ["sdl", "x11", "wayland", "macos", "windows"],
        description: "Target client platform to build.",
      },
      build_type: {
        type: "string",
        enum: ["Debug", "Release", "RelWithDebInfo"],
        default: "RelWithDebInfo",
        description: "CMake build type.",
      },
      build_dir: {
        type: "string",
        description:
          "Build directory path (relative to repo root). Defaults to build-<target>.",
      },
      mode: {
        type: "string",
        enum: ["dev", "full"],
        default: "dev",
        description:
          '"dev" runs cmake configure + build. "full" runs platform bundle scripts where available.',
      },
    },
    required: ["target"],
  },
  handler: async (params) => {
    const target = params.target;
    const buildType = params.build_type || "RelWithDebInfo";
    const buildDir = params.build_dir || `build-${target}`;
    const mode = params.mode || "dev";
    const preset = PRESET_MAP[target];

    if (target === "wayland") {
      session.log(
        "⚠️  Warning: the Wayland client is unmaintained upstream. Expect rough edges."
      );
    }

    // Full mode with platform-specific bundle scripts
    if (mode === "full") {
      if (target === "macos") {
        session.log("Running macOS bundle script…");
        const { stdout, stderr } = await run("bash", [
          join(REPO_ROOT, "scripts/bundle-mac-os.sh"),
        ]);
        return `macOS full build complete.\n\n${stdout}\n${stderr}`.trim();
      }
      if (target === "windows") {
        session.log("Running MinGW build script…");
        const { stdout, stderr } = await run("bash", [
          join(REPO_ROOT, "scripts/mingw.sh"),
        ]);
        return `Windows (MinGW) full build complete.\n\n${stdout}\n${stderr}`.trim();
      }
      session.log(
        `No full-mode script for "${target}" — falling back to dev mode.`
      );
    }

    // Dev mode (or full-mode fallback)
    const absBuildDir = join(REPO_ROOT, buildDir);

    session.log(`Configuring ${target} (${buildType}) in ${buildDir}…`);
    try {
      await run("cmake", [
        "-GNinja",
        `-DCMAKE_BUILD_TYPE=${buildType}`,
        `-C`,
        preset,
        `-B`,
        absBuildDir,
        `-S`,
        REPO_ROOT,
      ]);
    } catch (err) {
      return {
        textResultForLlm:
          `CMake configure failed for target "${target}".\n\n` +
          (err.stderr || err.stdout || err.message) +
          "\n\nCheck that the required dependencies are installed. " +
          "See the project README or ci/ scripts for package lists.",
        resultType: "error",
      };
    }

    session.log(`Building ${target}…`);
    try {
      await run("cmake", ["--build", absBuildDir, "--parallel"]);
    } catch (err) {
      return {
        textResultForLlm:
          `Build failed for target "${target}" in ${buildDir}.\n\n` +
          (err.stderr || err.stdout || err.message) +
          "\n\nFix the compilation errors above, then re-run the build.",
        resultType: "error",
      };
    }

    return `Build succeeded for target "${target}" (${buildType}).\nArtifacts are in: ${buildDir}`;
  },
};

// ── Tool 3: freerdp_test ────────────────────────────────────────────────────

const freerdpTest = {
  name: "freerdp_test",
  description:
    "Run the FreeRDP test suite via ctest. Optionally filter tests by regex.",
  parameters: {
    type: "object",
    properties: {
      build_dir: {
        type: "string",
        description:
          "Build directory containing CTestTestfile.cmake. Auto-detected if omitted.",
      },
      test_filter: {
        type: "string",
        description: "Regex filter passed to ctest -R to select specific tests.",
      },
      target: {
        type: "string",
        description:
          "Target name used to auto-detect build dir as build-<target>.",
      },
    },
  },
  handler: async (params) => {
    // Determine build directory: explicit > build-<target> > build-sdl > build
    let buildDir = params.build_dir;
    if (!buildDir && params.target) {
      buildDir = `build-${params.target}`;
    }

    if (!buildDir) {
      // Auto-detect: try build-sdl first, then build
      for (const candidate of ["build-sdl", "build"]) {
        try {
          await access(join(REPO_ROOT, candidate));
          buildDir = candidate;
          break;
        } catch {
          // not found, try next
        }
      }
    }

    if (!buildDir) {
      return {
        textResultForLlm:
          "Could not find a build directory. No build-sdl or build directory exists.\n\n" +
          "Build the project first with the freerdp_build tool, or pass an explicit build_dir.",
        resultType: "error",
      };
    }

    const absBuildDir = join(REPO_ROOT, buildDir);
    try {
      await access(absBuildDir);
    } catch {
      return {
        textResultForLlm:
          `Build directory "${buildDir}" does not exist.\n\n` +
          "Build the project first with the freerdp_build tool, or pass a valid build_dir.",
        resultType: "error",
      };
    }

    const ctestArgs = ["--test-dir", absBuildDir, "--output-on-failure"];
    if (params.test_filter) {
      ctestArgs.push("-R", params.test_filter);
    }

    session.log(
      `Running ctest in ${buildDir}` +
        (params.test_filter ? ` (filter: ${params.test_filter})` : "") +
        "…"
    );

    try {
      const { stdout, stderr } = await run("ctest", ctestArgs);
      return `Tests passed.\n\n${stdout}\n${stderr}`.trim();
    } catch (err) {
      const output = [err.stdout, err.stderr].filter(Boolean).join("\n");
      return {
        textResultForLlm:
          `Some tests failed in ${buildDir}.\n\n${output}\n\n` +
          "Review the failures above. Use test_filter to re-run specific tests.",
        resultType: "error",
      };
    }
  },
};

// ── Register ────────────────────────────────────────────────────────────────

session = await joinSession({
  tools: [freerdpSync, freerdpBuild, freerdpTest],
});
