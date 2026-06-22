#pragma once

void serialLibraryPortalBegin();
bool serialLibraryPortalLoop();
bool serialLibraryPortalIsBusy();
bool serialLibraryPortalIsConnected();
bool serialLibraryPortalConsumeConnectedEvent();
bool serialLibraryPortalConsumeDisconnectedEvent();
