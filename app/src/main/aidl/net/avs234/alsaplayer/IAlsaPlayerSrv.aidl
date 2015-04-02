package net.avs234.alsaplayer;

import net.avs234.alsaplayer.IAlsaPlayerSrvCallback;

interface IAlsaPlayerSrv {
	boolean init_playlist(in String path, int nitems);
	boolean add_to_playlist(in String track_source, in String track_name, int start_time, int pos);
	boolean play(int n, int start);
	boolean seek_to(int p);
	boolean play_next();
	boolean play_prev();
	boolean pause();
	boolean resume();
	boolean stop();
	boolean inc_vol();
	boolean dec_vol();
	boolean shutdown();
	boolean is_running();
	boolean is_paused();
	boolean initialized();
	boolean set_device(int card, int dev);
	boolean in_alsa_playback();
	int 	get_cur_mode();
	String  get_cur_dir();
	int	get_cur_pos();
	String	get_devinfo();
	String  get_cur_track_source();
	int	get_cur_seconds();
	int	get_track_duration();
	int	get_cur_track_len();
	int	get_cur_track_start();
	String  get_cur_track_name();
	void	set_headset_mode(int mode);
	void	registerCallback(IAlsaPlayerSrvCallback cb);
	void	unregisterCallback(IAlsaPlayerSrvCallback cb);
	int []	get_cue_from_flac(in String file);
	void	launch(in String path);
}
