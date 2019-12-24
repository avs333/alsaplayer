package net.avs234.alsaplayer;

import java.io.DataOutputStream;
import java.io.*;
import java.util.Timer;
import java.util.TimerTask;
import java.util.List;
import java.util.ArrayList;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.media.AudioManager;
import android.media.MediaPlayer;
import android.net.Uri;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.Process;
import android.os.RemoteCallbackList;
import android.os.RemoteException;
import android.os.SystemClock;
import android.os.Handler;
import android.telephony.PhoneStateListener;
import android.telephony.TelephonyManager;
import android.util.Log;
import android.view.KeyEvent;

import android.database.ContentObserver;

import eu.chainfire.libsuperuser.Shell;
import net.avs234.alsaplayer.util.AssetsUtils;
// import net.avs234.alsaplayer.SettingsContentObserver;
// import android.support.v4.app.NotificationCompat.Builder;

import android.content.ComponentName;
import android.support.v4.media.VolumeProviderCompat;
import android.support.v4.media.session.MediaSessionCompat; 
import android.support.v4.media.session.PlaybackStateCompat; 
import android.media.MediaDataSource;
import android.os.AsyncTask;

/*
TODO: 
1. Aidl needs to be changed to support runtime card switching.
2. audioInit must be called as early as possible on startup. To this end, card/device settings
   must be known from the app (either by reading its preferences directly, or via aidl).
   audioInit should also pass arrays of mixer setting parameters.
*/

public class AlsaPlayerSrv extends Service {
	
	static {
	    System.loadLibrary("lossless");
	}

	public static native long 	audioInit(long ctx, int card, int device);
	public static native boolean	audioExit(long ctx);
	public static native boolean	audioStop(long ctx);
	public static native boolean	audioPause(long ctx);
	public static native boolean	audioResume(long ctx);
	public static native boolean	audioOnScreenOff(long ctx);
	
	public static native int	audioGetDuration(long ctx);
	public static native int	audioGetCurPosition(long ctx);
	public static native boolean	audioIncreaseVolume(long ctx);
	public static native boolean	audioDecreaseVolume(long ctx);
	
	public static native int	audioPlay(long ctx, String file, int format, int start);
	public static native boolean	inOffloadMode(long ctx);	 

	public static native int []	extractFlacCUE(String file);

	public static native String []  getAlsaDevices();
	public static native String	getCurrentDeviceInfo(long ctx);

	public static native boolean	isUsbCard(int card); 
	public static native boolean	isOffloadDevice(int card, int device); 
	
	public static native boolean	libInit(int sdk);
	public static native boolean	libExit();
	

	// errors returned by xxxPlay functions
	public static final int LIBLOSSLESS_ERR_NOCTX = 1;
	public static final int LIBLOSSLESS_ERR_INV_PARM  =  2;
	public static final int LIBLOSSLESS_ERR_NOFILE = 3;
	public static final int LIBLOSSLESS_ERR_FORMAT = 4;
	public static final int LIBLOSSLESS_ERR_AU_GETCONF = 6;
	public static final int LIBLOSSLESS_ERR_AU_SETCONF = 7;
	public static final int LIBLOSSLESS_ERR_AU_BUFF = 8;
	public static final int LIBLOSSLESS_ERR_AU_SETUP = 9;
	public static final int LIBLOSSLESS_ERR_AU_START = 10;
	public static final int LIBLOSSLESS_ERR_IO_WRITE = 11;
	public static final int LIBLOSSLESS_ERR_IO_READ = 12;
	public static final int LIBLOSSLESS_ERR_DECODE = 13;
	public static final int LIBLOSSLESS_ERR_OFFSET = 14;
	public static final int LIBLOSSLESS_ERR_NOMEM = 15;
	
	// Audio context descriptor returned by audioInit(). 
	// Actually, it's a pointer to struct playback_ctx, see native code.
	// Used in all subsequent calls of native functions.
	private static long ctx = 0;

	
	public static final int MODE_NONE = 0;
	public static final int MODE_ALSA = 1;

	public static final int FORMAT_WAV = 0;
	public static final int FORMAT_FLAC = 1;
	public static final int FORMAT_APE = 2;
	public static final int FORMAT_MP3 = 3;
	public static final int FORMAT_ALAC = 4;
	
	// The lock to acquire so as the device won't go to sleep when we'are playing.  
	private PowerManager.WakeLock wakeLock = null;
	private NotificationManager nm = null;

	// Superuser shell
	private static Shell.Interactive suShell = null;
	
	private static void log_msg(String msg) {
		Log.i("AlsaPlayerSrv", msg);
	}
	private static void log_err(String msg) {
		Log.e("AlsaPlayerSrv", msg);
	}

	// process headset insert/remove events
	public static final int HANDLE_HEADSET_INSERT = 1;
	public static final int HANDLE_HEADSET_REMOVE = 2;
	
	public static final String ACTION_VIEW = "alsaPlayer_view";
	
	private static int headset_mode = 0;
	
	public static int curTrackLen = 0;
	public static int curTrackStart = 0;
	private static int last_cue_start = 0;
	private static int total_cue_len = 0;

	private VolumeProviderCompat vol_provider = null;
	private MediaSessionCompat media_session = null;

	// Callback to be called from native code

	public static void updateTrackLen(int time) {
		if(last_cue_start == -1) {
			total_cue_len = time;	// track time already updated from the cue, just save total length for the last track  
			return;	
		}
		curTrackLen = time - last_cue_start;
	}

	// Callback used to send new track name or error status to the interface thread.
	
	private static final RemoteCallbackList<IAlsaPlayerSrvCallback> cBacks = new RemoteCallbackList<IAlsaPlayerSrvCallback>();
	
	private void informTrack(String s, boolean error) {
	    final int k = cBacks.beginBroadcast();
	    for (int i=0; i < k; i++) {
	         try { 
	            	if(!error) {
	            		cBacks.getBroadcastItem(i).playItemChanged(false,s);
						AlsaPlayerSrv.this.notify(R.drawable.play_on/*R.drawable.playbackstart*/,s);
	            	} else {
	            		cBacks.getBroadcastItem(i).playItemChanged(true,getString(R.string.strStopped));
	            		if(s.compareTo(getString(R.string.strStopped))!= 0) cBacks.getBroadcastItem(i).errorReported(s);
	            	}
	         } catch (RemoteException e) { 
	        	 log_err("remote exception in informTrack(): " + e.toString());
	        	 break;
	         }
	    }
	    cBacks.finishBroadcast();
	}

	private void informPauseResume(boolean pause) {
		final int k = cBacks.beginBroadcast();
		for (int i=0; i < k; i++) {
	         try { 
	        	 cBacks.getBroadcastItem(i).playItemPaused(pause);
	         } catch (RemoteException e) { 
	        	 log_err("remote exception in informPauseResume(): " + e.toString());
	        	 break;
	         }
	    }
	    cBacks.finishBroadcast();
	}
	
	private MediaPlayer mplayer = null;
	private Object mplayer_lock= new Object();
	private int fck_start;
	private boolean isPrepared;
	public int extPlay(String file, int start) {
	 	final boolean zerofile = (file == null);
		try {
			isPrepared = false;
			fck_start = start;
			if(file != null) {
			    mplayer = new MediaPlayer();
			    mplayer.setDataSource(file);
			} else {
			    mplayer = MediaPlayer.create(getApplicationContext(), R.raw.silence);
			}
			if(mplayer == null) return LIBLOSSLESS_ERR_NOCTX;
			if(file == null) {
			    mplayer.setVolume(0,0);
			    mplayer.setLooping(true);
			}
			mplayer.setOnErrorListener(new MediaPlayer.OnErrorListener() {
				public boolean onError(MediaPlayer mp, int what, int extra)  {
					synchronized(mplayer_lock) {
						if(mplayer != null) mplayer.release(); 	mplayer = null;
					}
					log_err("mplayer playback aborted with errors: " + what + ", " + extra);
					informTrack(getString(R.string.strMplayerError),true);
					synchronized(mplayer_lock) {
						mplayer_lock.notify();
					}
					return false;
				}
			});
			mplayer.setOnCompletionListener(new MediaPlayer.OnCompletionListener() {
				public void onCompletion(MediaPlayer mp) {
					log_msg("mplayer playback completed");
					synchronized(mplayer_lock) {
						mplayer_lock.notify();
					}
				}
			});
			mplayer.setOnPreparedListener(new MediaPlayer.OnPreparedListener() {
				public void onPrepared(MediaPlayer mp) {
				    isPrepared = true;
				    if(!zerofile) {	
					if(fck_start != 0) mplayer.seekTo(fck_start*1000);
					curTrackLen = mplayer.getDuration()/1000;
					mplayer.start();
				    }	
				}
			});
			if(file == null) mplayer.start();			
			else mplayer.prepare();
			//SystemClock.sleep(250);
			//if(!mplayer.isPlaying()) return LIBLOSSLESS_ERR_INV_PARM;
			if(file == null) log_msg("silence: exited");
			synchronized(mplayer_lock) {
				mplayer_lock.wait();
			}
		} catch (Exception e) { 
	       	 	log_err("Exception in extPlay(): " + e.toString());
			e.printStackTrace();
	       	 	return LIBLOSSLESS_ERR_INV_PARM;
		} finally  {
			synchronized(mplayer_lock) {
				if(mplayer != null) {
					if(mplayer.isPlaying()) mplayer.stop();
					mplayer.release();
				}
				mplayer = null;
			}
		}
		return 0;
	}


	private static final int NOTIFY_ID = R.drawable.icon;	
	private void notify(int icon, String s) {
		if(nm == null) return;
		if(s == null) {
			nm.cancel(NOTIFY_ID);
			return;
		}
		Intent intent = new Intent();
		intent.setAction("android.intent.action.MAIN");
		intent.addCategory("android.intent.category.LAUNCHER");
		intent.setClass(this, AlsaPlayer.class);
		intent.setFlags(0x10100000);
//		final Notification notty = new Notification(icon, s, System.currentTimeMillis());	// 1
//		notty.setLatestEventInfo(getApplicationContext(), "alsaPlayer", s, 			// 2
//					PendingIntent.getActivity(this, 0, intent, 0));

		final Notification notty = new Notification.Builder(this)
			.setSmallIcon(icon).setTicker(s).setWhen(System.currentTimeMillis())		// 1
			.setContentTitle("alsaPlayer").setContentText(s).setContentIntent(PendingIntent.getActivity(this, 0, intent, 0)) // 2
			.build();



//		Notification.Action na = new Notification.Action.Builder(icon, "alsaPlayer", PendingIntent.getActivity(this, 0, intent, 0)).build();
//		Notification notty = new NotificationCompat.Builder(this).
//			setContentTitle("alsaPlayer").setContentText(s).setSmallIcon(icon).setActions(na).build();

		nm.notify(NOTIFY_ID,notty);
	}
	
	////////////////////////////////////////////////////////////////
	//////////////////////// Main class ////////////////////////////
	////////////////////////////////////////////////////////////////
		
	private class playlist {
			
		private String 	 	dir;		// source file(s) path
		private String[]	files;		// track source files
		private String[]	names;		// track names from cue files 
		private int[]		times; 		// track start times from cue files
		private	int 		cur_pos;	// current track
		private int		cur_start;	// start seconds the file must be played
		private PlayThread 	th;			// main thread
		private boolean		running;	// either playing or paused
		private boolean		paused;
		private CueUpdater	cup;		// updater for cue playlists
		private int		cur_mode;		// MODE_NONE for mp3 etc., MODE_ALSA for lossless files
		
		public boolean init_playlist(String path, int items) {
			if(items <= 0) return false;

		////////////////////////////////
			if(th != null) {
				log_msg("thread active, stop first");	
				stop();
			}
		///////////////////////////////
			dir = null;  	files = null;
			cur_pos = -1;	th = null;
			paused = false;	running = false; 
			times = null;	names = null;
			cup = null;		
			cur_mode = MODE_NONE;
			if(path != null) dir = new String(path);
			try {
				files = new String[items];
				names = new String[items];
				times = new int[items];
			} catch (Exception e) {
				log_err("exception in init_playlist(): " + e.toString());
				return false;
			}
			return true;
		}	
		
		public boolean add_to_playlist(String track_source, String track_name, int start_time, int pos) {
			if(pos >= files.length) return false;
			if(track_source == null) return false;
			try {
				files[pos] = new String(track_source); 
				if(track_name != null) names[pos] = new String(track_name); else names[pos] = null;
				times[pos] = start_time;
			} catch (Exception e) {
				log_err("exception in add_to_playlist(): " + e.toString());
				return false;
			}
			return true;
		}
		
		// Used for cue files to update current track number and name.
		private class CueUpdater {
			private class CUETimerTask extends TimerTask {
				public void run() {
					cur_pos++; cur_start = 0;
					if(cur_pos < names.length && names[cur_pos] != null) {
						if(cur_pos + 1 < files.length) curTrackLen = times[cur_pos+1] - times[cur_pos];
						else curTrackLen = total_cue_len - times[cur_pos];
						curTrackStart = getCurPosition(); // audioGetCurPosition(ctx);
						log_msg("track name = " + names[cur_pos] + ", curTrackLen=" + curTrackLen + ", curTrackStart=" + curTrackStart);
						informTrack(names[cur_pos],false);
					}
					if(cur_pos + 1 < names.length) schedule((times[cur_pos+1] - times[cur_pos])*1000);
				}
			}	
			private Timer timer;
			private CUETimerTask timer_task;
			private long last_time_left;
			private long last_time_start;
			
			public void shutdown() {
				if(timer_task != null) timer_task.cancel();
				if(timer != null) timer.cancel();
			}
			private void reset() {
				shutdown();
				timer_task = new CUETimerTask();
				timer = new Timer();
			}
			public synchronized void schedule(long delay) {
				reset();
				last_time_left = delay;
				last_time_start = System.currentTimeMillis();
				timer.schedule(timer_task, delay);
			}
			public synchronized void pause() {
				reset();
				last_time_left -= (System.currentTimeMillis() - last_time_start); 
			}
			public synchronized void resume() {
				last_time_start = System.currentTimeMillis();
				timer.schedule(timer_task, last_time_left);
			}
		}
				
		private int  cur_card = 0;
		private int  cur_device = 0;
	
		private boolean initAudioCard(int card_no, int dev_no) {
			if(checkSetDevicePermissions(card_no, dev_no)) {
					log_msg("checkSetDevicePermissions() returned OK");
				} else {
					log_msg("checkSetDevicePermissions() returned error!");
					return false;
				}
			
			try {
				log_msg("trying to init: old_ctx=" + String.format("0x%x",ctx) + " card hw:" + card_no + "," + dev_no);
		   		ctx = audioInit(ctx, card_no, dev_no);
	   		} catch(Exception e) { 
		   		log_err("exception in audioInit(): " + e.toString());
		   		return false;
	   		}
	   		if(ctx == 0) {
		   		log_err("audioInit() failed");
		   		return false;
	   		}
			log_msg("initAuidoCard context=" + String.format("0x%x",ctx));
		        cur_mode = MODE_ALSA;
			cur_card = card_no;
			cur_device = dev_no;
	           	return true;
		}
			
		private int getCurPosition() {
		//	if(!running) return 0;
			if(cur_mode == MODE_NONE) {
				if(mplayer != null) return mplayer.getCurrentPosition()/1000;
			} 
			if(ctx == 0) return 0;
			return audioGetCurPosition(ctx) + cur_start;
		}
	
		private int getDuration() {
		//	if(!running) return 0;
			if(cur_mode == MODE_NONE) {
				do {
					SystemClock.sleep(200);
			        } while (!isPrepared);
				if(mplayer != null && isPrepared) return mplayer.getDuration()/1000;
			} 
			if(ctx == 0) return 0;
			return audioGetDuration(ctx);
		}

		private class SilentPlayerTask extends AsyncTask<String,Integer,String> {
			@Override
			protected String doInBackground(String... args) {
			    log_msg("starting silence");	
			    int k = extPlay(null, 0);	
			    log_msg("silence terminated with code " + k);	
			    return "done";	
			}
		/*	@Override
			protected void onPostExecute( String dummy ) {
			    this.onCancelled();
			}  */
			@Override
			public void onCancelled() {
			     log_msg("silence cancelled");	
			}			
		}

		private SilentPlayerTask sp_task = null;
	
		int alsaPlay(String file, int format, int start) {
		    cur_mode = MODE_ALSA;
	//	    sp_task = new SilentPlayerTask();
	//	    sp_task.execute(); 
		    if(ctx == 0) ctx = audioInit(0, cur_card, cur_device);
		    if(ctx == 0) return 1;
		    return audioPlay(ctx, file, format, start);
		}
						
		private class PlayThread extends Thread {
			private int tid = -1;
			public void run() {
				tid = Process.myTid();
				running = true;
				log_msg("run(): starting new thread " + tid + ", ctx = " + String.format("0x%x", ctx));
				Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);
				if(!wakeLock.isHeld()) wakeLock.acquire();
				int k;
				for(k = 1; running && cur_pos < files.length; cur_pos++) {
					log_msg(Process.myTid() + ": trying " + files[cur_pos] + " @ time " + (times[cur_pos] + cur_start));
					try {
						curTrackLen = 0;
						curTrackStart = 0;
						last_cue_start = 0;
						total_cue_len = 0;
						if(names[cur_pos] != null) {	// this is CUE track
							log_msg("track name = " + names[cur_pos]);
							if(cur_pos + 1 < files.length) {
								informTrack(names[cur_pos],false);
								if(times[cur_pos+1] > times[cur_pos]) {
									curTrackLen = times[cur_pos+1]-times[cur_pos]; 
									last_cue_start = -1;						   
									cup = new CueUpdater();
									cup.schedule((times[cur_pos+1] - times[cur_pos])*1000 - cur_start*1000);
								}
							} else {	// last CUE track				
								informTrack(names[cur_pos],false);	
								last_cue_start = times[cur_pos]; // native thread will subtract this from the total cue length 
							}
						} else {
							String cur_file = files[cur_pos];
							int start = cur_file.lastIndexOf("/") + 1; 
							int end = cur_file.lastIndexOf(".");
							String cf = end > start ? cur_file.substring(start,end) : cur_file.substring(start);
							informTrack(cf,false);
						}
						if(files[cur_pos].endsWith(".flac") || files[cur_pos].endsWith(".FLAC")) {
							k = alsaPlay(files[cur_pos], FORMAT_FLAC, times[cur_pos]+cur_start);
						} else if(files[cur_pos].endsWith(".ape") || files[cur_pos].endsWith(".APE")) {
							k = alsaPlay(files[cur_pos], FORMAT_APE, times[cur_pos]+cur_start);
						} else if(files[cur_pos].endsWith(".wav") || files[cur_pos].endsWith(".WAV")) {
							k = alsaPlay(files[cur_pos], FORMAT_WAV, times[cur_pos]+cur_start);
						} else if(files[cur_pos].endsWith(".m4a") || files[cur_pos].endsWith(".M4A")) {
							k = alsaPlay(files[cur_pos], FORMAT_ALAC, times[cur_pos]+cur_start);
						} else if(files[cur_pos].endsWith(".mp3") || files[cur_pos].endsWith(".MP3")) {
							if(ctx == 0) ctx = audioInit(0, cur_card, cur_device);
							boolean offload = (ctx != 0) ? inOffloadMode(ctx) : false;
							if(!offload) {
								cur_mode = MODE_NONE;	
								k = extPlay(files[cur_pos],times[cur_pos]+cur_start);
							} else {
								k = alsaPlay(files[cur_pos], FORMAT_MP3, times[cur_pos]+cur_start);
							}
			              		} else {
							cur_mode = MODE_NONE;	
							k = extPlay(files[cur_pos],times[cur_pos]+cur_start);
						}
			              		nm.cancel(NOTIFY_ID);
					} catch(Exception e) { 
						log_err("run(): exception in xxxPlay(): " + e.toString());
						cur_start = 0;
						continue;
					}
					cur_start = 0;
					if(k == 0) log_msg(Process.myTid() + ": xxxPlay() returned normally");
					else {
						log_err(String.format("run(): xxxPlay() returned error %d",k));
						running = false;
						String err, s[] = getResources().getStringArray(R.array.Errors);
							try  {
								err = s[k-1];
							} catch(Exception e) {
								err = getString(R.string.strInternalError);
							}
							informTrack(err,true);
						break;
					}
					if(names[0]!= null) break; // just in case
				}
				if(wakeLock.isHeld()) wakeLock.release();
				log_msg(Process.myTid() + ": thread about to exit");
				if(k == 0) informTrack(getString(R.string.strStopped),true);
				try {
					if(cup != null) cup.shutdown();
				} catch (Exception e) {
					log_err("Timer exception in run(): " + e.toString());
				}
				cup = null;
				running = false;
			}
			public int getThreadId() {
				return tid;
			}
		}		
		public boolean stop() {
			running = false;
			log_msg("stop()");
			nm.cancel(NOTIFY_ID);
			if(th != null) { 
				int i = Process.getThreadPriority(Process.myTid()); 
				int tid = th.getThreadId();
				int k = 0;
				
				try {
					if(cup != null) cup.shutdown();
				} catch (Exception e) {
					log_err("Timer exception in stop(): " + e.toString());
				}
				cup = null;
				log_msg(String.format("stop(): terminating thread %d from %d", tid, Process.myTid()));
				Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
				if(cur_mode == MODE_ALSA) {
					log_msg("stopping context " + String.format("0x%x", ctx));
					audioStop(ctx);
				}
				synchronized(mplayer_lock) {	// always
					mplayer_lock.notify();
				}
				if(sp_task != null) {
					sp_task.cancel(true);
					sp_task = null;
				}
				if(paused) paused = false;
				try {
					while(th.isAlive()) {	
						th.join(100); k++;
					/*	if(th.isAlive()) SystemClock.sleep(50);
						else break; */
						if(k > 20) break;
					}
				} catch (InterruptedException e) {
					log_err("Interrupted exception in stop(): " + e.toString());
				}
				if(th.isAlive()) {
					log_err(String.format("stop(): thread %d is still alive after %d ms",tid, k*100));
					informTrack(getString(R.string.strSrvFail),true);
					try {
						th.join();
					} catch (InterruptedException e) {
						log_err("Interrupted exception in stop#th.join(): " + e.toString());
					}
				} else log_msg(String.format("stop(): thread terminated after %d ms",k*100));
				th = null;
				Process.setThreadPriority(i);
			} else log_msg(String.format("stop(): player thread was null (my tid %d)", Process.myTid()));
			return true;
		}
		public boolean play(int n, int start) {
			log_msg(String.format("play(%d)",n));
			stop();
			if(files == null || n >= files.length || n < 0) return false;
			cur_pos= n; cur_start = start;
			th = new PlayThread();
			log_msg(String.format("play(): created new thread from %d", Process.myTid()));
			th.start();
			return true;
		}
		public boolean seekTo(int p) {
			log_msg(String.format("seekTo(%d)",p));
			mplayer.seekTo(p);
			return true;
		}
		public boolean play_next() {
			log_msg("play_next()");
			return play(cur_pos+1,0);
		}
		public boolean play_prev() {
			log_msg("play_prev()");
			return play(cur_pos-1,0);
		}
		public boolean pause() {
			log_msg("pause()");
			if(files == null || paused) return false;
				
			if(cur_mode == MODE_NONE) {
				paused = true;
				try {
					mplayer.pause();					
				} catch (Exception e) {
					log_err("Mplayer exception in pause(): " + e.toString());
					paused = false;
				}
			} else {
				int i = Process.getThreadPriority(Process.myTid()); 
				Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
				if(audioPause(ctx)) paused = true;
				Process.setThreadPriority(i);
			}
			if(wakeLock.isHeld()) wakeLock.release();
			if(cup != null) cup.pause();
			return paused == true;
		}
		public boolean resume() {
			log_msg("resume()");
			if(files == null || !paused) return false;
			if(cur_mode == MODE_NONE) {
				paused = false;
				try {
					mplayer.start();					
				} catch (Exception e) {
					log_err("Mplayer exception in resume(): " + e.toString());
					paused = true;
				}
			} else {
				int i = Process.getThreadPriority(Process.myTid()); 
				Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
				if(audioResume(ctx)) paused = false;
				Process.setThreadPriority(i);
			}

			if(!paused && !wakeLock.isHeld()) wakeLock.acquire();
			if(cup != null) cup.resume();
			return paused == false;
		}
		public boolean dec_vol() {
			log_msg("dec_vol()");
			if(files == null || !running) return false;
			if(cur_mode == MODE_NONE) return true;
			int i = Process.getThreadPriority(Process.myTid()); 
			Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
			boolean ret = audioDecreaseVolume(ctx);
			Process.setThreadPriority(i);
			return ret;
		}
		public boolean inc_vol() {
			log_msg("inc_vol()");
			if(files == null || !running) return false;
			if(cur_mode == MODE_NONE) return true;
			int i = Process.getThreadPriority(Process.myTid()); 
			Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
			boolean ret = audioIncreaseVolume(ctx);
			Process.setThreadPriority(i);
			return ret;
		}
		public boolean set_device(int card, int device) {
			if(initAudioCard(card, device)) {
				cur_card = card;
				cur_device = device;
				log_msg("card " + card + ":" + device + " initialised");
				return true;
			}
			log_msg("failed to init card " + card + ":" + device);
			return false;
		}
		public int get_mode() {
		    return cur_mode;
		}
	}
	
	private static playlist plist = null;

	//////////////////////////////////////////////////
	//// The interface we expose to clients.  It's returned to them when the connection is established. 

	private static final IAlsaPlayerSrv.Stub binder = new IAlsaPlayerSrv.Stub() {
		public boolean init_playlist(String path, int nitems) {
			return plist.init_playlist(path,nitems); 
		}
		public boolean  add_to_playlist(String src, String name, int start_time, int pos) { return plist.add_to_playlist(src,name,start_time,pos);}
		public boolean	play (int n, int start) 	{ return plist.play(n,start); }
		public boolean	seek_to (int p) { return plist.seekTo(p); }
		public boolean	play_next()  	{ return plist.play_next(); }
		public boolean	play_prev()  	{ return plist.play_prev(); }
		public boolean	pause() 	{ return plist.pause(); }
		public boolean	resume() 	{ return plist.resume(); }
		public boolean	stop() 		{ return plist.stop(); }
		public boolean	inc_vol() 	{ return plist.inc_vol(); }
		public boolean	dec_vol() 	{ return plist.dec_vol(); }
		public boolean	shutdown() 	{ plist.stop();  if(ctx != 0) audioExit(ctx); ctx = 0; return true; }
		public boolean	is_running()	{ return plist.running; }
		public boolean  initialized() 	{ return ctx != 0; }	// why this function? 
		public boolean	is_paused()	{ return plist.paused; }
		public boolean	set_device(int card, int dev)	{ return plist.set_device(card, dev); }
		public boolean  in_alsa_playback() { return plist.running && plist.cur_mode == MODE_ALSA; }
		public int	get_cur_mode()	{ return plist.cur_mode; }
		public String	get_cur_dir()	{ return plist.dir; }
		public int	get_cur_pos()	{ return plist.cur_pos; }
		public String	get_devinfo()	{ return (ctx != 0) ? getCurrentDeviceInfo(ctx) : null; }
		public int	get_cur_seconds()		{ return plist.getCurPosition(); }
		public int	get_track_duration()		{ return plist.getDuration(); }
		public int	get_cur_track_start() 	{ return curTrackStart; }
		public int	get_cur_track_len() 	{ return curTrackLen; }
		public String  	get_cur_track_source()	{ try { return plist.files[plist.cur_pos]; } catch(Exception e) {return null;} }
		public String  	get_cur_track_name()	{ try { return plist.names[plist.cur_pos]; } catch(Exception e) {return null;} }
		public void	set_headset_mode(int m)	{ headset_mode = m; }
		public void 	registerCallback(IAlsaPlayerSrvCallback cb)   { if(cb != null) cBacks.register(cb); };
		public void 	unregisterCallback(IAlsaPlayerSrvCallback cb) { if(cb != null) cBacks.unregister(cb); };
		public int []	get_cue_from_flac(String file) 	{ return  extractFlacCUE(file); };
		public void	launch(String path)		{ if(launcher != null) launcher.launch(path);  };
	};
	
	private class Launcher {
		void launch(String path) {	startActivity((new Intent()).setAction(AlsaPlayerSrv.ACTION_VIEW).setData(Uri.fromFile(new File(path))));	}
	}
	static Launcher launcher = null;
	
	///////////////////////////////////////////////////////
	///////////////////// Overrides ///////////////////////
	
	@Override
	public IBinder onBind(Intent intent) {
		log_msg("onBind()");	
		return binder;
	}

	@Override
	public void onCreate() {
		   	super.onCreate();
		   	log_msg("onCreate()");
	        registerPhoneListener();
	        registerHeadsetReciever();
	        if(wakeLock == null) {
	        	PowerManager pm = (PowerManager)getSystemService(Context.POWER_SERVICE);
	        	wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,
				//	android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON, 
					this.getClass().getName());
	        	wakeLock.setReferenceCounted(false);
	        }
	        plist = new AlsaPlayerSrv.playlist();
	        launcher = new Launcher();
	        if(nm == null) nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
	        Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);

	        if(!libInit(Integer.parseInt(Build.VERSION.SDK))) {	        	
	        	log_err("cannot initialize atrack library");
	        	stopSelf();
	        }

		AssetsUtils.loadAsset(this, "cards.xml", ".alsaplayer/cards.xml", false);

	/*
		AudioManager aman = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
		ComponentName receiver = new ComponentName(getPackageName(), MediaButtonIntentReceiver.class.getName());
		aman.registerMediaButtonEventReceiver(receiver);
		media_session = new MediaSessionCompat(this, "AlsaPlayerSrv", receiver, null);
	*/

		media_session = new MediaSessionCompat(this, "AlsaPlayerSrv");


		media_session.setFlags(MediaSessionCompat.FLAG_HANDLES_MEDIA_BUTTONS 
			| MediaSessionCompat.FLAG_HANDLES_TRANSPORT_CONTROLS);

		media_session.setCallback(new MediaSessionCompat.Callback(){ 
	/*	    @Override
		    public void onPlay() { log_msg("onPlay callback"); }
		    @Override
		    public void onPause() { log_msg("onPause callback"); }
		    @Override
		    public void onStop() { log_msg("onStop callback"); } */
		    @Override
		    public boolean onMediaButtonEvent(Intent mediaButtonIntent) { 
			log_msg("onMediaButtonEvent callback received");
			return false; 
		    }
		});

	//	PlaybackStateCompat pstate = null;
	//	PlaybackStateCompat.Builder cbl = new PlaybackStateCompat.Builder();

	//	cbl.setActions(PlaybackStateCompat.ACTION_PLAY|PlaybackStateCompat.ACTION_PLAY_PAUSE|PlaybackStateCompat.ACTION_STOP);

	//	cbl.setState(PlaybackStateCompat.STATE_PLAYING, PlaybackStateCompat.PLAYBACK_POSITION_UNKNOWN, 0);
	//	pstate = cbl.build();

		PlaybackStateCompat pstate = new PlaybackStateCompat.Builder()
			.setState(PlaybackStateCompat.STATE_PLAYING, PlaybackStateCompat.PLAYBACK_POSITION_UNKNOWN, 0).build();	

		media_session.setPlaybackState(pstate);
	//	log_msg("session created, state: " + pstate.getState());

		vol_provider = new VolumeProviderCompat(VolumeProviderCompat.VOLUME_CONTROL_RELATIVE, 100, 50) {
		    @Override
		    public void onAdjustVolume(int direction) {
			if(direction != 0 && plist != null) {
				if(direction > 0) { log_msg("volume increase"); plist.inc_vol(); }
				else { log_msg("volume decrease"); plist.dec_vol(); }
			}
		    }
		};

		media_session.setPlaybackToRemote(vol_provider);
		media_session.setActive(true);
	}

	@Override
	public int onStartCommand(Intent intent, int flags, int startId) {
	    boolean perf_mode = intent.getBooleanExtra("codec_perf_mode", false);
	    log_msg("onStartCommand called with perf_mode = " + perf_mode);
	    setPermissions(perf_mode); 	
	    // We want this service to continue running until it is explicitly
	    // stopped, so return sticky.
	    return START_STICKY;
	}

		
	@Override
	public void onDestroy() {
		log_msg("onDestroy()");
	//	cBacks.kill();
		unregisterPhoneListener();
		unregisterHeadsetReciever();
		if(ctx != 0) audioExit(ctx);
	        if(wakeLock != null && wakeLock.isHeld()) wakeLock.release();
	        if(nm != null) nm.cancel(NOTIFY_ID);
		if(suShell != null) suShell.close();
		media_session.setActive(false);
		media_session.release();
	        libExit();
	}	 
	
	@Override
	public boolean onUnbind(Intent intent) {
		log_msg("onUnbind()");
	//	if(nm != null) nm.cancel(NOTIFY_ID);
		return super.onUnbind(intent);
	}

	///////////////////////////////////////////////////////
	//////////// Headset Connection Detection /////////////
	
	private BroadcastReceiver headsetReciever = new BroadcastReceiver() {
		private boolean needResume = false;
		@Override
		public void onReceive(Context context, Intent intent) {

                        if(intent.getAction().equalsIgnoreCase(Intent.ACTION_SCREEN_ON)) {
                                log_msg("SCREEN ON");
                        } else if(intent.getAction().equalsIgnoreCase(Intent.ACTION_SCREEN_OFF)) {
                                log_msg("SCREEN OFF");
				if(plist != null && plist.running && plist.get_mode() == MODE_ALSA && ctx != 0) audioOnScreenOff(ctx);
                        } else if (isIntentHeadsetRemoved(intent)) {
				log_msg("Headset Removed: " + intent.getAction());
				if(plist != null && plist.running && !plist.paused) {
					if(((headset_mode & HANDLE_HEADSET_REMOVE) != 0)) {
						plist.pause();
						informPauseResume(true);
					}
					needResume = true;
				}
			} else if (isIntentHeadsetInserted(intent)) {
				log_msg("Headset Inserted: " + intent.getAction());
				if(needResume) {
					if(plist != null && (headset_mode & HANDLE_HEADSET_INSERT) != 0) {
						plist.resume();
						informPauseResume(false);
					}
					needResume = false;
				}
			}
		}
		
		private boolean isIntentHeadsetInserted(Intent intent) {
			return (intent.getAction().equalsIgnoreCase(Intent.ACTION_HEADSET_PLUG)
					&& intent.getIntExtra("state", 0) != 0);
		}
		
		private boolean isIntentHeadsetRemoved(Intent intent) {
			return ((intent.getAction().equalsIgnoreCase(Intent.ACTION_HEADSET_PLUG)
					&& intent.getIntExtra("state", 0) == 0) 
					|| intent.getAction().equalsIgnoreCase(AudioManager.ACTION_AUDIO_BECOMING_NOISY));
		}
	};
	
	private void registerHeadsetReciever() {
		IntentFilter filter = new IntentFilter();
		filter.addAction(Intent.ACTION_SCREEN_ON);
		filter.addAction(Intent.ACTION_SCREEN_OFF);
		filter.addAction(Intent.ACTION_HEADSET_PLUG);
		filter.addAction(AudioManager.ACTION_AUDIO_BECOMING_NOISY);
		registerReceiver(headsetReciever, filter);
	}
	
	private void unregisterHeadsetReciever() {
		unregisterReceiver(headsetReciever);
	}
	
	////////////////////////////////////////////////////////
	//////// Pause when the phone rings, and resume after the call

	private PhoneStateListener phoneStateListener = new PhoneStateListener() {
	     private boolean needResume = false;
		 @Override
	     public void onCallStateChanged(int state, String incomingNumber) {
	            if (state == TelephonyManager.CALL_STATE_RINGING || state == TelephonyManager.CALL_STATE_OFFHOOK) {
	            	if(plist != null && plist.running && !plist.paused) {
	            		plist.pause();	
	            		informPauseResume(true);
	            		needResume = true;
	            	}
	            } else if (state == TelephonyManager.CALL_STATE_IDLE) {
	            	if(needResume) {
	            		if(plist != null) {
	            			plist.resume();
	            			informPauseResume(false);
	            		}
	            		needResume = false;
	            	}
	            }
	     }
	};
	
	private void registerPhoneListener() {
		TelephonyManager tmgr = (TelephonyManager) getSystemService(Context.TELEPHONY_SERVICE);
		tmgr.listen(phoneStateListener, PhoneStateListener.LISTEN_CALL_STATE);
	}
	
	private void unregisterPhoneListener() {
		TelephonyManager tmgr = (TelephonyManager) getSystemService(Context.TELEPHONY_SERVICE);
		tmgr.listen(phoneStateListener, 0);
	}

	////////////////////////////////////////////////////
	//////////  Access-related stuff.

	public synchronized static boolean checkSetDevicePermissions(int card, int device) {

		log_msg("checkSetDevicePermissions entry");
		boolean okay;

		// Setup control device first: it's needed for isOffloadDevice() call
		// required to determine playback device name

		String devpath = "/dev/snd/controlC" + card;
       		File dev = new File(devpath);

		if(suShell == null) suShell = (new Shell.Builder()).useSU().open();
	
		if(!dev.canRead() || !dev.canWrite()) {
			try {
				suShell.addCommand(new String[] {
                                        "chcon u:object_r:device:s0 /dev/snd",
                                        "chcon u:object_r:zero_device:s0 " + devpath,
                                        "chmod 0666 " + devpath,
                                });
				suShell.waitForIdle();
				dev = new File(devpath);
				okay = dev.canRead() && dev.canWrite();
				if(!okay) {
					log_msg("Failed to setup " + devpath);
					log_msg("checkSetDevicePermissions exit");
					return false;
				} 
				log_msg("Device " + devpath + " prepared" );
			} catch (Exception e) {
	                        log_err("exception while setting control device permissions: " + e.toString()); 
				log_msg("checkSetDevicePermissions exit");
        	                return false; 
			}
		} else log_msg("No need to prepare " + devpath);

		// Now, playback device.

		boolean offload = isOffloadDevice(card, device);

		devpath = offload ? "/dev/snd/comprC" : "/dev/snd/pcmC";
		devpath += card;
		devpath += "D";
		devpath += device;
		if(!offload) devpath += "p";
		dev = new File(devpath);

		if(dev.canRead() && dev.canWrite()) {
			log_msg("No need to prepare " + devpath);
			log_msg("checkSetDevicePermissions exit");
			return true;
		}
		try {	
			suShell.addCommand(new String[] {
				"chcon u:object_r:zero_device:s0 " + devpath,
				"chmod 0666 " + devpath
			});
			suShell.waitForIdle();
	
		} catch (Exception e) {
			log_err("exception while setting audio device permissions: " + e.toString());
			log_msg("checkSetDevicePermissions exit");
			return false;
		}
		dev = new File(devpath);
		okay = dev.canRead() && dev.canWrite();

		if(!okay) log_msg("Failed to prepare " + devpath);
		else log_msg("Device " + devpath + " prepared");
		log_msg("checkSetDevicePermissions exit");
		return okay;
	}

	public synchronized static String [] getDevices() {

	    log_msg("getDevices entry");

	    int card = 0;

	    if(suShell == null) suShell = (new Shell.Builder()).useSU().open();

	    /* make sure that controls for all currently connected devices are prepared */
	    while(true) {	
		String devpath = "/dev/snd/controlC" + card;
		File ctl = new File(devpath);
		if(!ctl.exists()){
		    if(card == 0) {
			log_err("no mixer for first card, exiting");
	    		log_msg("getDevices exit");
			return null;
		    }	 
		    break;
		}
		card++;
		if(ctl.canRead() && ctl.canWrite()) continue;
		try {
			suShell.addCommand(new String[] {
				"chmod 0666 " + devpath,
				"chcon u:object_r:zero_device:s0 " + devpath
			});
			suShell.waitForIdle();
		} catch(Exception e) {
		    log_err("exception in getDevices");
	    	    log_msg("getDevices exit");
		    return null;	
		}
	    }
	    log_msg("getDevices: premissions set for " + card + (card == 1 ? " card" : " cards"));
	    log_msg("getDevices exit");
	    return getAlsaDevices();
	}


	public synchronized static boolean setPermissions(boolean perf_mode) {

	    log_msg("setPermissions entry");

	    boolean result = false;
	    File g = null;

		if(perf_mode) {
		    g = new File("/sys/module/snd_soc_wcd9330/parameters/high_perf_mode");
		    if(!g.exists()) {
		    	g = new File("/sys/module/snd_soc_wcd9320/parameters/high_perf_mode");
			if(!g.exists()) g = null;
		    }
		}	 	

		if(suShell == null) suShell = (new Shell.Builder()).useSU().open();

	        try {
		    	log_msg("setting selinux to permissive mode & /dev/snd permissions");	
			if(g != null) {
			    suShell.addCommand(new String[] {
	            		"/system/bin/setenforce 0",
		    		"/system/bin/chmod 0777 /dev/snd",
				"/system/bin/chcon u:object_r:device:s0 /dev/snd",
				"echo 1 > " + g.toString()
			    });
			} else {
			    suShell.addCommand(new String[] {
	            		"/system/bin/setenforce 0",
		    		"/system/bin/chmod 0777 /dev/snd",
				"/system/bin/chcon u:object_r:device:s0 /dev/snd"
			    });
			}
			suShell.waitForIdle();

			log_msg("setting devices permissions/context");	
			File [] devices = (new File("/dev/snd")).listFiles();

		 	if(devices != null) {
			    List<String> commands = new ArrayList<String> (); 	
			    for(int i = 0; i < devices.length; i++) {
				commands.add("/system/bin/chmod 0666 " + devices[i].toString());
				commands.add("/system/bin/chcon u:object_r:zero_device:s0 " + devices[i].toString());
			    }
			    suShell.addCommand(commands);
			} else log_err("failed to read /dev/snd now");

			suShell.waitForIdle();

			log_msg("checking results");	
			File f = new File("/dev/snd/controlC0");
			if(f.canRead() && f.canWrite()) {
			      	result = true;
				log_msg("control device readable/writable");
			} else log_err("control device not readable or writable");	
			f = new File("/dev/snd/pcmC0D0p");
			if(f.canRead() && f.canWrite()) log_msg("first device readable/writable too");
			else log_err("first device not readable or writable");	
	        } catch (Exception e) {
			log_err("got exception: " + e.toString());
	        }
		log_msg("setPermissions exit: returning " + result);
            return result;
	}

/*
	public synchronized static boolean setPermissions(boolean perf_mode) {
	    boolean result = false;
	    java.lang.Process process = null;
	    DataOutputStream os = null;
	    File g = null;
		log_msg("setPermissions entry");
		if(perf_mode) {
		    g = new File("/sys/module/snd_soc_wcd9330/parameters/high_perf_mode");
		    if(!g.exists()) {
		    	g = new File("/sys/module/snd_soc_wcd9320/parameters/high_perf_mode");
			if(!g.exists()) g = null;
		    }
		}	 	

	        try {
        	    process = new ProcessBuilder("su").redirectErrorStream(true).start();
	            os = new DataOutputStream(process.getOutputStream());

		    log_msg("setting selinux to permissive mode");	
	            os.write(("/system/bin/setenforce 0\n").getBytes());
	            os.flush();

		    log_msg("setting /dev/snd permissions/context");	
		    os.write(("/system/bin/chmod 0777 /dev/snd\n").getBytes());
		    os.flush();	
                    os.write(("/system/bin/chcon u:object_r:device:s0 /dev/snd\n").getBytes());
		    os.flush();
		    if(g != null) {
			log_msg("setting module param for " + g.toString());
			os.write(("echo 1 > " + g.toString() + "\n").getBytes());
			os.flush();
		    } else log_msg("no perf_mode module param to set");		

	            os.write(("exit\n").getBytes());
	            os.flush();
		    log_msg("waiting for process to exit");
	            process.waitFor();

		    process.destroy();	
		    os.close();		
			
        	    process = new ProcessBuilder("su").redirectErrorStream(true).start();
	            os = new DataOutputStream(process.getOutputStream());

		    log_msg("setting devices permissions/context");	
		    File [] devices = (new File("/dev/snd")).listFiles();
		    if(devices != null) {
			for(int i = 0; i < devices.length; i++) {
			//	log_msg("device " + i + " " + devices[i]);
			    	os.write(("/system/bin/chmod 0666 " + devices[i].toString() + "\n").getBytes());
				os.flush();
				os.write(("/system/bin/chcon u:object_r:zero_device:s0 " + devices[i].toString() + "\n").getBytes());
				os.flush();
			}
		    } else log_err("failed to read /dev/snd now");

	            os.write(("exit\n").getBytes());
	            os.flush();
		    log_msg("waiting for process to exit");
	            process.waitFor();


		    log_msg("checking results");	
		    File f = new File("/dev/snd/controlC0");
		    if(f.canRead() && f.canWrite()) {
	            	result = true;
			log_msg("control device readable/writable");
		    } else log_err("control device not readable or writable");	
		    f = new File("/dev/snd/pcmC0D0p");
		    if(f.canRead() && f.canWrite()) log_msg("first device readable/writable too");
		    else log_err("first device not readable or writable");	
	        } catch (Exception e) {
			log_err("got exception: " + e.toString());
	        } finally {
	            if (process != null) {
	                process.destroy();
	            }
	            try {
	                if (os != null) {
	                    os.close();
	                }
	            } catch (IOException e) {
	            }
	        }
		log_msg("setPermissions exit: returning " + result);
            return result;
	} 
*/

}
