package net.avs234.alsaplayer;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;
import android.view.KeyEvent;

public class RemoteControlReceiver extends BroadcastReceiver {

    @Override
    public void onReceive(Context context, Intent intent) {
        // Log.i("AlsaPlayer.RemoteControlReceiver", "intent: " + intent);
        if (Intent.ACTION_MEDIA_BUTTON.equals(intent.getAction())) {
            IBinder iBinder = this.peekService(context, new Intent(context,
                    AlsaPlayerSrv.class));
            // Log.i("AlsaPlayer.RemoteControlReceiver", "iBinder" + iBinder);
            IAlsaPlayerSrv srv = IAlsaPlayerSrv.Stub.asInterface(iBinder);
            ;
            if (srv == null) {
                Log.e("AlsaPlayer.RemoteControlReceiver", "srv==null");
                return;
            }
            // Log.i("AlsaPlayer.RemoteControlReceiver", "srv" + srv);
            KeyEvent event = (KeyEvent) intent
                    .getParcelableExtra(Intent.EXTRA_KEY_EVENT);

            if (event == null || event.getAction() != KeyEvent.ACTION_DOWN)
                return;
            try {
            	Log.i("AlsaPlayer.RemoteControlReceiver", "event " + event + " code "+ event.getKeyCode());
                switch (event.getKeyCode()) {
                case KeyEvent.KEYCODE_MEDIA_STOP:
                    srv.pause();
                    break;
                case KeyEvent.KEYCODE_HEADSETHOOK:
                case KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE:
                    if (srv.is_running() && !srv.is_paused())
                        srv.pause();
                    else
                        srv.resume();
                    break;
                case KeyEvent.KEYCODE_MEDIA_NEXT:
                    srv.play_next();
                    break;
                case KeyEvent.KEYCODE_MEDIA_PREVIOUS:
                    srv.play_prev();
                    break;
                }
            } catch (Exception e) {
            }

            this.abortBroadcast();

        }
    }
}
