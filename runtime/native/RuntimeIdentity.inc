// Run only the native module, placement and release selected by the formal entry.
void runtimeState(const wchar_t* name,const char* phase) {
 mark(name,object({{"nonce",quote(runtimeNonce)},{"session",quote(runtimeRoot)},
  {"pid",std::to_string(GetCurrentProcessId())},{"phase",quote(std::string(phase))}}));
}
void verifyRuntimeIdentity(const std::wstring& ini,const std::wstring& module) {
 const auto expectedRelease=iniValue(ini,L"release");
 require(utf8(expectedRelease)==eruntime::releaseSHA,"runtime release differs from compiled source");
 require(_wcsicmp(iniValue(ini,L"module").c_str(),module.c_str())==0,"runtime module path mismatch");
 const auto moduleSHA=sha256(module);
 require(moduleSHA==utf8(iniValue(ini,L"moduleSHA")),"loaded binary differs from build receipt");
 const auto placementSHA=sha256(at(L"placement.tsv"));
 require(placementSHA==utf8(iniValue(ini,L"placementSHA")),"runtime placement differs from prepared input");
 mark(L"loaded-module.json",object({{"release",quote(std::string(eruntime::releaseSHA))},
  {"version",quote(std::string(eruntime::releaseVersion))},{"module",quote(module)},
  {"moduleSHA",quote(moduleSHA)},{"placementSHA",quote(placementSHA)}}));
}
