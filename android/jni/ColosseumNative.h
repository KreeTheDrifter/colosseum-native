// Colosseum Native for Android: hooks called from MainAndroid.cpp.
#pragma once

namespace ColosseumNative
{
// Before BootManager::BootCore: StaticRecomp core with the recompiled game code, in-game menu.
void BeforeBoot();

// For Host_UIBlocksControllerState: the game ignores the controller while the menu is open.
bool MenuIsOpen();
}  // namespace ColosseumNative
