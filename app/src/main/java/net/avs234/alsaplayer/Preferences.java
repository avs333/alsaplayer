package net.avs234.alsaplayer;

import android.app.AlertDialog;
import android.app.Dialog;
import android.os.Bundle;
import android.preference.CheckBoxPreference;
import android.preference.ListPreference;
import android.preference.Preference;
import android.preference.PreferenceActivity;
import android.preference.PreferenceCategory;
import android.preference.PreferenceScreen;
import android.preference.Preference.OnPreferenceClickListener;
import java.io.*;
import java.util.*;
import android.util.Log;

public class Preferences extends PreferenceActivity {

    private void log_msg(String msg) {
	Log.i(getClass().getSimpleName(), msg);
    }

    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setPreferenceScreen(createPreferenceHierarchy());
    }
    
    @Override
	protected Dialog onCreateDialog(int id) {
		switch (id) {
		case 0:
            return new AlertDialog.Builder(this)
                .setIcon(R.drawable.new_icon)
                .setTitle(R.string.app_name)
                .setMessage(R.string.strAbout)
                .create();
		}
		return null;
	}

    private PreferenceScreen createPreferenceHierarchy() {
        PreferenceScreen root = getPreferenceManager().createPreferenceScreen(this);
        root.setTitle(getString(R.string.app_name)+" "+getString(R.string.strSettings));
        
        PreferenceCategory launchPrefCat = new PreferenceCategory(this);
        launchPrefCat.setTitle(R.string.strBSettings);
        root.addPreference(launchPrefCat);
        
        CheckBoxPreference shuffle_mode = new CheckBoxPreference(this);
        shuffle_mode.setTitle(R.string.strShuffle);
        shuffle_mode.setKey("shuffle_mode");
        launchPrefCat.addPreference(shuffle_mode);
        
      /*  CheckBoxPreference driver_mode = new CheckBoxPreference(this);
        driver_mode.setTitle(R.string.strDriverMode);
        driver_mode.setKey("driver_mode");
        launchPrefCat.addPreference(driver_mode); */
        
        CheckBoxPreference book_mode = new CheckBoxPreference(this);
        book_mode.setTitle(R.string.strSaveBooks);
        book_mode.setKey("book_mode");
        launchPrefCat.addPreference(book_mode);

        CheckBoxPreference hs_remove_mode = new CheckBoxPreference(this);
        hs_remove_mode.setTitle(R.string.strHsRemove);
        hs_remove_mode.setKey("hs_remove_mode");
        launchPrefCat.addPreference(hs_remove_mode);

        CheckBoxPreference hs_insert_mode = new CheckBoxPreference(this);
        hs_insert_mode.setTitle(R.string.strHsInsert);
        hs_insert_mode.setKey("hs_insert_mode");
        launchPrefCat.addPreference(hs_insert_mode);

	/* ************************* */
	ArrayList<String> ents = new ArrayList<String>();
	ArrayList<String> vals = new ArrayList<String>();

	PreferenceCategory devsCat = new PreferenceCategory(this);
        devsCat.setTitle(R.string.strDevSel);
        root.addPreference(devsCat);
/*     
	ListPreference card = new ListPreference(this);
	card.setTitle(R.string.strCardName);
	card.setKey("card_num");
	for(i = 0; (new File("/proc/asound/card" + i)).exists(); i++) {
		ents.add("card" + i); vals.add(Integer.toString(k));
	}
	card.setEntries(ents.toArray(new String[ents.size()]));
	card.setEntryValues(vals.toArray(new String[vals.size()]));
	devsCat.addPreference(card);

	ents.clear();
	vals.clear(); */

	ListPreference device = new ListPreference(this);
	device.setTitle(R.string.strDeviceName);
	device.setKey("device");
	try {

/*		FileReader fr = new FileReader("/proc/asound/pcm");
		BufferedReader br = new BufferedReader(fr);
		String s;
		int i = 1;	
		while((s = br.readLine()) != null) { 
			if(!s.contains("playback ")) continue;
			if(s.charAt(7) == '(') continue;
			int end = s.substring(7).indexOf(":") + 7;
			ents.add(s.substring(7,end));
			vals.add(s.substring(0,5));
			i++;
		} */
		int i;
		String s;
		log_msg("calling AlsaPlayerSrv.getDevices()");
		String devices[] = AlsaPlayerSrv.getDevices();
		log_msg("devices.length=" + devices.length);
		for(i = 0; i < devices.length; i++) {
			s = devices[i];
			log_msg("found " + s);
			ents.add(s.substring(7));
			vals.add(s.substring(0,5));
		}
	} catch (Exception e) { log_msg("exception!"); e.printStackTrace(); }

	device.setEntries(ents.toArray(new String[ents.size()]));
	device.setEntryValues(vals.toArray(new String[vals.size()]));
	devsCat.addPreference(device);

	if(device.getValue() == null) {
		log_msg("value not set, setting zero");
		device.setValueIndex(0);
	}

	/* ************************* */
        
        PreferenceCategory alsaplayerPrefCat = new PreferenceCategory(this);
        alsaplayerPrefCat.setTitle(R.string.app_name);
        root.addPreference(alsaplayerPrefCat);
        
        PreferenceScreen alsaplayerPrefAbout = getPreferenceManager().createPreferenceScreen(this);
        alsaplayerPrefAbout.setOnPreferenceClickListener(new OnPreferenceClickListener() {
			
			public boolean onPreferenceClick(Preference p) {
				showDialog(0);
				return false;
			}
        });
        alsaplayerPrefAbout.setTitle(R.string.strAbout1);
        alsaplayerPrefCat.addPreference(alsaplayerPrefAbout);
        return root;
    }
}
