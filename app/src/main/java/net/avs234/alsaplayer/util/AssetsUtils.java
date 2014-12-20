package net.avs234.alsaplayer.util;

import android.content.Context;
import android.os.Environment;

import java.io.*;

/**
 * Created by skvalex on 20/12/14.
 */
public class AssetsUtils {
    public static boolean loadAsset(Context context, String path, String to, boolean replace) {
        File file = new File(Environment.getExternalStorageDirectory().getAbsolutePath() + "/" + to);
        file.getParentFile().mkdirs();
        if (file.exists() && !replace) {
            return false;
        }
        file.delete();

        byte[] buffer = new byte[1024];
        InputStream inputStream = null;
        OutputStream outputStream = null;
        try {
            inputStream = context.getResources().getAssets().open(path);
            outputStream = new FileOutputStream(file);
            int read;
            while ((read = inputStream.read(buffer)) != -1) {
                outputStream.write(buffer, 0, read);
            }
            outputStream.flush();
        } catch (FileNotFoundException e) {
            e.printStackTrace();
        } catch (IOException e) {
            e.printStackTrace();
        } finally {
            if (inputStream != null) {
                try {
                    inputStream.close();
                } catch (IOException e) {
                    e.printStackTrace();
                }
            }
            if (outputStream != null) {
                try {
                    outputStream.close();
                } catch (IOException e) {
                    e.printStackTrace();
                }
            }
            inputStream = null;
            outputStream = null;
        }
        return true;
    }
}
