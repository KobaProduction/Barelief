#pragma once

class CoreApplication;

// Opt-in, passive delivery evidence for the actual installed Release executable.
void install_package_verification_probe(CoreApplication& app);
