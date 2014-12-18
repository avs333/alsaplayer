package net.avs234.alsaplayer;

oneway interface IAlsaPlayerSrvCallback {
    void playItemChanged(boolean error, String name);
	void errorReported(String name);
	void playItemPaused(boolean paused);
}
