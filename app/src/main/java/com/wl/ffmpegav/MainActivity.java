package com.wl.ffmpegav;

import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import android.Manifest;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.Environment;
import android.util.Log;
import android.view.View;
import android.widget.Button;

import java.io.File;

public class MainActivity extends AppCompatActivity {

    private final String TAG = "FFmpegAV";

    private final static String SRC_AUDIO_PATH = Environment.getExternalStorageDirectory() + File.separator + "filefilm" + File.separator + "mediatest2.mp4";
    private final static String SRC_VIDEO_PATH = Environment.getExternalStorageDirectory() + File.separator + "filefilm" + File.separator + "mediatest2.mp4";
    private final static String DST_AUDIO_PATH = Environment.getExternalStorageDirectory() + File.separator + "filefilm" + File.separator + "mediatest2.aac";
    private final static String DST_VIDEO_PATH = Environment.getExternalStorageDirectory() + File.separator + "filefilm" + File.separator + "mediatest2.h264";
    private final static String AV_MP4_TO_FLV_DST_PATH = Environment.getExternalStorageDirectory() + File.separator + "filefilm" + File.separator + "mediatest2.flv";
    private final static String CLIP_DST_PATH = Environment.getExternalStorageDirectory() + File.separator + "filefilm" + File.separator + "clip10s.mp4";
    private final static String MERGE_AUDIO_FILE = Environment.getExternalStorageDirectory() + File.separator + "filefilm" + File.separator + "mediatest2.aac";
    private final static String MERGE_VIDEO_FILE = Environment.getExternalStorageDirectory() + File.separator + "filefilm" + File.separator + "mediatest2.h264";
    private final static String MERGE_DST_FILE = Environment.getExternalStorageDirectory() + File.separator + "filefilm" + File.separator + "merge_av.mp4";

    private final static String SRC_YUV_TO_H264 = Environment.getExternalStorageDirectory() + File.separator + "filefilm" + File.separator + "out_raw_video.yuv";
    private final static String DST_YUV_TO_H264 = Environment.getExternalStorageDirectory() + File.separator + "filefilm" + File.separator + "out_raw_video.h264";
    private final static String DST_YUV = Environment.getExternalStorageDirectory() + File.separator + "filefilm" + File.separator + "out_raw_video.yuv";
    private final static String DST_PCM = Environment.getExternalStorageDirectory() + File.separator + "filefilm" + File.separator + "out_raw_video.pcm";
    private final static String SRC_DECODE_PATH = Environment.getExternalStorageDirectory() + File.separator + "filefilm" + File.separator + "mediatest2.mp4";
    private final static String SRC_VIDEO_IMAGE_PATH = Environment.getExternalStorageDirectory() + File.separator + "filefilm" + File.separator + "mediatest3.mp4";
    private Button mAudioExtract;
    private Button mVideoExtract;
    private Button mAVFormatConversion;
    private Button mAVClip;
    private Button mAVMerge;
    private Button mAudioEncode;
    private Button mAudioDecode;
    private Button mVideoEncode;
    private Button mVideoDecode;
    private Button mVideoToImg;

    // Used to load the 'native-lib' library on application startup.
    static {
        System.loadLibrary("native-lib");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        mAudioExtract = findViewById(R.id.audio_extraction);
        mAudioExtract.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                audioExtraction(SRC_AUDIO_PATH, DST_AUDIO_PATH);
            }
        });

        mVideoExtract = findViewById(R.id.video_extraction);
        mVideoExtract.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                videoExtraction(SRC_VIDEO_PATH, DST_VIDEO_PATH);
            }
        });

        mAVFormatConversion = findViewById(R.id.av_format_conversation);
        mAVFormatConversion.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                audioVideoFormatMp4ToFlv(SRC_AUDIO_PATH, AV_MP4_TO_FLV_DST_PATH);
            }
        });

        mAVClip = findViewById(R.id.av_clip);
        mAVClip.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                audioVideoClip(SRC_AUDIO_PATH, CLIP_DST_PATH, 10, 25);
            }
        });

        mAVMerge = findViewById(R.id.av_merge);
        mAVMerge.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                audioVideoMerge(MERGE_AUDIO_FILE, MERGE_VIDEO_FILE, MERGE_DST_FILE);
            }
        });

        mVideoToImg = findViewById(R.id.av_to_img);
        mVideoToImg.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                new Thread(new Runnable() {
                    @Override
                    public void run() {
                        videoToImg(SRC_VIDEO_IMAGE_PATH);
                    }
                }).start();
            }
        });

        mVideoEncode = findViewById(R.id.video_encode);
        mVideoEncode.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                videoFileEncode(SRC_YUV_TO_H264, DST_YUV_TO_H264);
            }
        });

        mVideoDecode = findViewById(R.id.video_decode);
        mVideoDecode.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                videoFileDecode(SRC_DECODE_PATH, DST_YUV);
            }
        });

        mAudioEncode = findViewById(R.id.audio_encode);
        mAudioEncode.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                audioFileEncode(DST_PCM, DST_AUDIO_PATH);
            }
        });

        mAudioDecode = findViewById(R.id.audio_decode);
        mAudioDecode.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Log.d(TAG, "onClick: audioFileDecode");
                audioFileDecode(SRC_DECODE_PATH, DST_PCM);
            }
        });

    }

    private void requestMyPermissions() {
        if (ContextCompat.checkSelfPermission(this,
                Manifest.permission.WRITE_EXTERNAL_STORAGE)
                != PackageManager.PERMISSION_GRANTED) {
            //没有授权，编写申请权限代码
            Log.d(TAG, "requestMyPermissions: 请求写SD权限");
            ActivityCompat.requestPermissions(MainActivity.this, new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE}, 100);
        } else {
            Log.d(TAG, "requestMyPermissions: 有写SD权限");
        }
        if (ContextCompat.checkSelfPermission(this,
                Manifest.permission.READ_EXTERNAL_STORAGE)
                != PackageManager.PERMISSION_GRANTED) {
            //没有授权，编写申请权限代码
            Log.d(TAG, "requestMyPermissions: 请求读SD权限");
            ActivityCompat.requestPermissions(MainActivity.this, new String[]{Manifest.permission.READ_EXTERNAL_STORAGE}, 100);
        } else {
            Log.d(TAG, "requestMyPermissions: 有读SD权限");
        }
    }

    /**
     * A native method that is implemented by the 'native-lib' native library,
     * which is packaged with this application.
     */
    public native void audioExtraction(String srcfile, String dstfile);
    public native void videoExtraction(String srcfile, String dstfile);
    public native void audioVideoFormatMp4ToFlv(String srcfile, String dstfile);
    public native void audioVideoClip(String srcfile, String dstfile, int from_time, int stop_time);
    //音频文件：aac, 视频文件：h264
    public native void audioVideoMerge(String srcAudiofile, String srcVideofile, String dstfile);
    //编码音频文件 pcm
    public native void audioFileEncode(String srcPCMFile, String dstAACFile);
    public native void audioFileDecode(String srcMp4File, String dstPCMFile);
    //编码视频文件 yuv
    public native void videoFileEncode(String srcYUVFile, String dstH264File);
    public native void videoFileDecode(String srcMp4File, String dstYUVFile);
    public native void videoToImg(String srcMp4File);
}
