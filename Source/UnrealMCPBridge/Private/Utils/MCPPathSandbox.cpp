// Copyright FatumGame. All Rights Reserved.

#include "MCPPathSandbox.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
	/**
	 * Build the canonical whitelist root strings — invoked per call (cheap; FPaths::* just
	 * returns cached FStrings). We deliberately do NOT static-cache because the project paths
	 * are stable across a session but engine paths could shift under live-coding scenarios,
	 * and the cost of FPaths::ConvertRelativePathToFull on 4 strings is microseconds.
	 *
	 * The returned strings are guaranteed to:
	 *   - Be absolute (drive letter on Windows / leading '/' on POSIX)
	 *   - Use forward slashes (FPaths normalises)
	 *   - Have a trailing '/' (so a prefix check correctly differentiates ``ProjectDir/Foo``
	 *     from ``ProjectDirSibling/...``)
	 */
	TArray<FString> BuildWhitelistRoots()
	{
		TArray<FString> Roots;

		auto AddRoot = [&Roots](FString Root)
		{
			// FPaths::CollapseRelativeDirectories + ConvertRelativePathToFull together produce a
			// canonical absolute form; NormalizeDirectoryName drops any trailing slash so we
			// re-append a single '/' for the prefix-check below.
			Root = FPaths::ConvertRelativePathToFull(Root);
			FPaths::CollapseRelativeDirectories(Root);
			FPaths::NormalizeDirectoryName(Root);
			if (!Root.EndsWith(TEXT("/")))
			{
				Root += TEXT("/");
			}
			Roots.Add(MoveTemp(Root));
		};

		AddRoot(FPaths::ProjectDir());
		AddRoot(FPaths::ProjectSavedDir());
		AddRoot(FPaths::ProjectIntermediateDir());
		AddRoot(FPaths::EngineDir());

		return Roots;
	}

	/** Canonicalise an input path to absolute + collapsed + forward-slash form. */
	FString CanonicaliseAbs(const FString& InPath)
	{
		FString Path = InPath;
		Path.TrimStartAndEndInline();
		// ConvertRelativePathToFull treats a relative path as relative-to-CWD; for sandboxed
		// scripting we WANT it relative-to-project. So if the input is relative we anchor it
		// explicitly to ProjectDir.
		if (FPaths::IsRelative(Path))
		{
			Path = FPaths::Combine(FPaths::ProjectDir(), Path);
		}
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::CollapseRelativeDirectories(Path);
		// Normalise both directory separators and case folding (Windows-style); UE's FPaths
		// uses '/' as the canonical separator.
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		return Path;
	}
}

namespace FMCPPathSandbox
{

bool Resolve(const FString& InPathRaw, FString& OutAbsPath, FString& OutError)
{
	OutAbsPath.Reset();
	OutError.Reset();

	if (InPathRaw.IsEmpty())
	{
		OutError = TEXT("path is empty");
		return false;
	}

	// .. escape — reject before canonicalisation since Collapse may flatten them silently into
	// an in-sandbox path that hides intent ("Foo/../../Bar" → "Bar" which may be in-sandbox).
	if (InPathRaw.Contains(TEXT("..")))
	{
		OutError = TEXT("relative parent (..) segments not allowed in sandboxed paths");
		return false;
	}

	const FString Canonical = CanonicaliseAbs(InPathRaw);
	if (!IsInsideSandbox(Canonical))
	{
		OutError = FString::Printf(
			TEXT("path '%s' resolves to '%s' which is outside the sandbox whitelist "
				 "(project / saved / intermediate / engine)"),
			*InPathRaw, *Canonical);
		return false;
	}

	OutAbsPath = Canonical;
	return true;
}

bool IsInsideSandbox(const FString& AbsolutePath)
{
	const TArray<FString> Roots = BuildWhitelistRoots();
	for (const FString& Root : Roots)
	{
		// Prefix-check with case-insensitive semantics — Windows paths are case-preserving but
		// case-insensitive. POSIX-side this is technically over-permissive but the project lives
		// in a single mount so the false-positive surface is empty.
		if (AbsolutePath.StartsWith(Root, ESearchCase::IgnoreCase))
		{
			return true;
		}
		// Also accept an exact match on the root (sans trailing slash) for sandbox-root requests.
		FString RootNoSlash = Root;
		if (RootNoSlash.EndsWith(TEXT("/")))
		{
			RootNoSlash.LeftChopInline(1, EAllowShrinking::No);
		}
		if (AbsolutePath.Equals(RootNoSlash, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

} // namespace FMCPPathSandbox
