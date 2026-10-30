#pragma once

// Which Free Touch this is (2026-10-29).
// FT_SELF_UPDATE 1: the build shared by zip and published with tools/release.py, which updates itself from Free Touch's
// service (src/core/Updater).
// FT_SELF_UPDATE 0: the BakkesPlugins build, which BakkesMod updates. tools/bakkesplugins.py writes 0 here in its
// source zip and leaves the updater's files out.
#define FT_SELF_UPDATE 0
