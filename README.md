# Introduction

`callstream` is a multi-threaded plugin for [trunk-recorder](https://github.com/robotastic/trunk-recorder). This plugin streams complete audio calls recorded by trunk-recorder from conventional and trunked radio systems, such as local fire/rescue/EMS.

This plugin was designed to work with [pizzawave](https://github.com/lilhoser/pizzawave), a .NET application that transcribes the recorded WAV files to text using [OpenAI's Whisper AI model](https://openai.com/research/whisper) as exposed through [whisper.net toolchain](https://github.com/sandrohanea/whisper.net).

# Requirements
* Linux system running trunk-recorder

# Installation

***Note: To automate installation of this plugin, see [this bash script](https://github.com/lilhoser/pizzawave/scripts/setup_trunk_recorder.sh).***

* Copy the contents of this repo to a new folder, `trunk-recorder/user_plugins/callstream`
* [Rebuild trunk-recorder](https://trunkrecorder.com/docs/Install/INSTALL-LINUX)
```
cd trunk-build
cmake ../trunk-recorder
make
sudo make install
```

# Configure

Add the following code to your trunk-recorder's JSON configuration:

```json
"plugins": [
    {
        "name":"callstream",
        "library":"libcallstream.so",
        "clients":[
            {
                "address":"192.168.1.122",
                "port":9123
            }
        ],
        "streams":[
            {
                "TGID":0,
                "shortName":"<system name>"
            }
        ],
        "sftp_info":{
            "server_address": "<address>",
            "user": "<user>",
            "password": "<password>",
            "dest": "myfolder/mysubfolder",
            "verbose": false
        }
    }
]
```

Make sure you set the global `audioStreaming` to `true`.

* `clients`: specify up to 6 clients (address and port) that will receive the streamed calls from this plugin. The callstream plugin was designed to communicate with [pizzawave](https://github.com/lilhoser/pizzawave) application on the remote client, but you can easily write your own client and do whatever you want to with the call data.
* `System name`: this is the name of the trunk-recorder system from your configuration file.

If TGID is set to 0, all calls from all talkgroups will be sent. If TGID is set to a specific decimal value, only calls from that talkgroup will be sent.

## RF Telemetry

The optional `rf_telemetry` block emits passive, versioned JSON events through
the Trunk Recorder log. It does not retune a receiver or access SDR hardware.

```json
"rf_telemetry": {
    "enabled": true,
    "sample_interval_seconds": 15
}
```

Periodic samples and reacquisition events are prefixed with `PIZZAWAVE_RF`.
Matching Trunk Recorder builds emit retunes with the `TR_RF` prefix. Signal
power and noise are not included because Trunk Recorder does not currently
expose reliable control-channel measurements for those values.

## Audio Filtering

The callstream plugin includes optional audio filtering to reduce artifacts commonly found in P25 digital audio, such as sharp static pops/clicks caused by IMBE vocoder errors or bit errors in transmission.

Audio filtering is controlled via the `audio_filtering` configuration block:

```json
"audio_filtering": {
    "enabled": true,
    "spike_clipping": {
        "enabled": true,
        "threshold_percent": 85,
        "clip_factor": 0.9
    },
    "smoothing": {
        "enabled": false,
        "window_size": 5
    },
    "high_pass_filter": {
        "enabled": false,
        "cutoff_hz": 200
    }
}
```

### Filter Options

| Option | Description | Default |
|--------|-------------|---------|
| `enabled` | Enable/disable all audio filtering | `true` |
| **Spike Clipping** |||
| `spike_clipping.enabled` | Enable spike detection and soft clipping | `true` |
| `spike_clipping.threshold_percent` | Amplitude threshold as % of INT16_MAX (32767) | `85` |
| `spike_clipping.clip_factor` | How aggressively to clip spikes (0.0-1.0) | `0.9` |
| **Smoothing** |||
| `smoothing.enabled` | Enable moving average smoothing | `false` |
| `smoothing.window_size` | Smoothing strength (larger = smoother, less detail) | `5` |
| **High-Pass Filter** |||
| `high_pass_filter.enabled` | Enable high-pass filter to remove low-frequency tones | `false` |
| `high_pass_filter.cutoff_hz` | Cutoff frequency in Hz | `200` |

### Filter Descriptions

- **Spike Clipping**: Detects sudden amplitude spikes (typically caused by P25 decoding errors) and soft-clips them to reduce sharp clicks/pops. This is the most effective filter for P25 artifacts and is enabled by default.

- **Smoothing**: Applies exponential smoothing to reduce rough edges with less latency and smearing than a moving average. Disabled by default.

- **High-Pass Filter**: Removes low-frequency tonal artifacts (beeps, chirps) below the cutoff frequency. May affect bass content in voice audio. Disabled by default.

### Example Configuration

Here's a complete example with audio filtering enabled:

```json
"plugins": [
    {
        "name":"callstream",
        "library":"libcallstream.so",
        "clients":[
            {
                "address":"192.168.1.122",
                "port":9123
            }
        ],
        "streams":[
            {
                "TGID":0,
                "shortName":"EMS"
            }
        ],
        "audio_filtering": {
            "enabled": true,
            "spike_clipping": {
                "enabled": true,
                "threshold_percent": 85,
                "clip_factor": 0.9
            },
            "smoothing": {
                "enabled": false,
                "window_size": 5
            },
            "high_pass_filter": {
                "enabled": false,
                "cutoff_hz": 200
            }
        }
    }
]
```

You might consider disabling these [per-system settings](https://trunkrecorder.com/docs/CONFIGURE) in trunk-recorder, if you're not using them for another purpose:
* `audioArchive` - this setting controls whether or not trunk-recorder writes WAV files to disk after it processes calls. Since `callstream` sends the same data over the wire to your clients, this is wasted processing.
* `compressWav` - this setting controls whether WAVs are compressed before writing them to disk (something needed for other plugins like openmhz), which is unnecessary if `audioArchive` is disabled
* `transmissionArchive` - keep this disabled unless you are performing low-level diagnostics. Trunk Recorder still creates temporary per-transmission files while concluding a call; this setting controls whether those files are retained afterward. Callstream version 2 can use the temporary files to recover exact transmission audio when its live PCM buffer does not align.

The `sftp_info` block specifies an SFTP server to upload callstream records. This block is optional and can be removed. It is a useful option if you don't want to rely on a live streaming capability or would prefer to backup all callstream records for offline consumption (perhaps in addition to live streaming). In pizzawave parlance, this is known as an "offline capture".

# Run

Once you have rebuilt trunk-recorder with the plugin code and modified your trunk-recorder config to enable the plugin, you can run trunk-recorder to begin transmitting calls. In the console, you should see log messages prefixed with `callstream`. If you need to see more detailed diagnostics, bump up your log level in trunk-recorder config file's global `logLevel` setting.

# Data Protocol

If you're writing your own client to consume call data sent by this plugin, you will need to read the data as follows:

| Offset      | Length (bytes) |  Description            |
| ----------- | -------------- | ----------------------- |
| 0           | 4              | Magic 'PZZA'            |
| 4           | 8              | JSON length in bytes    |
| 12          | 4              | Number of audio samples |
| 16          | (variable)     | JSON data               |
| (variable)  | (variable)     | Sample data, int16 ea.  |

This data is guaranteed to be sent in this order.  Note at the time of writing, that the sample data is recorded by trunk-recorder as 16-bit, 1 channel, 8khz sampling rate.

Version 3 JSON structure extends version 2 without changing the binary framing:

| Field Name             | Description                                                        |
| ---------------------- | ------------------------------------------------------------------ |
| SchemaVersion          | Protocol version, currently `3`                                     |
| ChannelAssignmentStart | `grant` when the original assignment was observed; otherwise `update` |
| BeginsChannelAssignment | True only when `ChannelAssignmentStart` is `grant`                 |
| PossiblyIncompleteTransmissionStartTimeMs | Original first-transmission start for an update-created call; null for a grant |
| SystemNumber           | Trunk Recorder system number; this is not a transmitting radio ID  |
| SystemShortName        | The name of the system                                              |
| CallId                 | Trunk Recorder's parent call number                                |
| Talkgroup              | Parent talkgroup ID                                                |
| PatchedTalkgroups      | Array of patched talkgroups                                        |
| Frequency              | Parent call frequency                                             |
| StartTime / StopTime   | Unix seconds retained for version 1 client compatibility           |
| StartTimeMs / StopTimeMs | Unix milliseconds                                                |
| SampleRate             | PCM sample rate                                                    |
| AudioMappingStatus     | `exact_live`, `exact_reconstructed`, or `unavailable`              |
| Transmissions          | Ordered array of decoder-delimited push-to-talk transmissions      |

Each transmission contains `SourceId`, `SourceIdProvenance`, `StartStatus`, `Talkgroup`,
`StartTimeMs`, `StopTimeMs`, `StartSample`, `SampleCount`, `Frequency`,
`TdmaSlot`, `ErrorCount`, and `SpikeCount`. `SourceId` is null when the radio
identifier was not decoded. `StartSample` is null only when exact audio mapping
is unavailable. `StartStatus` is `possibly_incomplete` only for the first
transmission of a call created from an update; later transmissions are
`observed_boundary`.

Callstream omits a source-less transmission only when its temporary WAV is
successfully inspected and every sample remains at the observed decoder noise
floor. A source-less transmission with any signal above that floor is retained.
If inspection or exact reconstruction fails, Callstream retains the
transmission. When an empty fragment is omitted, Callstream reconstructs the
outgoing PCM so the transmission table still covers the complete payload.

For exact mappings, transmission ranges are contiguous, begin at sample zero,
and cover the entire PCM payload exactly. Callstream first compares its
in-memory PCM count with Trunk Recorder's retained transmission counts. On a
mismatch it reconstructs the outgoing PCM from Trunk Recorder's temporary
per-transmission WAV files and reapplies the configured Callstream filters. If
neither route can establish an exact mapping, the parent recording is still
sent with `AudioMappingStatus` set to `unavailable`; offsets are not invented.


# What's up with the name?
I dunno, I like pizza and Teenage Mutant Ninja Turtles, so it seemed to work.

# Notes for linux dev on VS Code

## Setting up VS Code for remote development

* install vscode
* install remote-ssh extension
* either create an ssh key or reuse your github key - export from puttygen as openssh format (pub)
* to avoid password prompting for the key on every operation, see [this page](https://code.visualstudio.com/docs/remote/troubleshooting)
    ```
    Set-Service ssh-agent -StartupType Automatic
    Start-Service ssh-agent
    Get-Service ssh-agent
    ```
    * from PS prompt: `ssh-add <path_to_your_ssh_public_key_file>`
    * restart vscode
    * make sure the remote host's vscode config looks like this:
    ```
    Host 192.168.1.173
	  User <user_name>
	  HostName <remote_ip_address>
	  IdentityFile "~/.ssh/<key_from_above>.pub"
	  ForwardAgent yes
    ```
* install cmake tools extension
    * setup CMakeFiles and "kit"
* install c++ tools extension

## Using gdb with VS Code

* Make sure it's a debug build `cmake . -DCMAKE_BUILD_TYPE=debug`
* setup `.vscode/launch.json` to look like this (from an example building `trunk-recorder`):
```
		{
			"version": "0.2.0",
			"configurations": [
				{
					"name": "(gdb) Launch trunk-recorder",
					"type": "cppdbg",
					"request": "launch",
					"program": "/home/<user>/trunk-build/trunk-recorder",
					"args": ["--config=sdr_config.json"],
					"stopAtEntry": false,
					"cwd": "/home/<user>/trunk-build",
					"environment": [],
					"externalConsole": false,
					"MIMode": "gdb",
					"setupCommands": [
						{
							"description": "Enable pretty-printing for gdb",
							"text": "-enable-pretty-printing",
							"ignoreFailures": true
						},
						{
							"description": "Set Disassembly Flavor to Intel",
							"text": "-gdb-set disassembly-flavor intel",
							"ignoreFailures": true
						}
					],
					"additionalSOLibSearchPath": "/home/<user>/trunk-build/",
					"preLaunchTask": "build"
				}
			],
		}
```
* use the C++ extension's "run and debug" sub-menu, beneath the "run and debug" tile on the left and click the green play button to start

# Self-signed certificates when using CURL and SFTP

If you're using the `sftp_info` configuration parameter for `callstream`, you'll want to read and understand this section.

## Problem and solution

Under the hood, `callstream` uses `libcurl` to communicate with the target SFTP server. In the situation where the SFTP server is using a self-signed certificate (common for home network setups), you will need to add the PEM certificate of all intermediate and root signing authorities to your client system's trusted cert store. For SSH, this location is either your user profile `~/.ssh/known_hosts` or systemwide `/etc/ssh/ssh_known_hosts`. Simply add the output of `ssh-keyscan -H <address>` to one of these files and restart SSH `sudo systemctl restart ssh`. To test that it worked, after restarting SSH, run `curl --user <user> sftp://<address> -debug`.

## Details

Without installing the necessary Certificate Authority (CA) certs on the client machine, curl will complain:

```	
lilhoser@omicrontheta:~/trunk-build$ curl --user pizzawave sftp://192.168.1.183 -debug
Enter host password for user 'pizzawave':
curl: (60) SSL peer certificate or SSH remote key was not OK
More details here: https://curl.se/docs/sslcerts.html

curl failed to verify the legitimacy of the server and therefore could not
establish a secure connection to it. To learn more about this situation and
how to fix it, please visit the web page mentioned above.
```

The issue is the server is responding to curl's SFTP handshake request with a self-signed certificate whose root (or intermediate CAs, if any) are not trusted by the requesting client machine. Because curl defaults to enforcing peer verification, the request will fail with [CURLE_PEER_FAILED_VALIDATION](https://curl.se/mail/lib-2020-07/0023.html). While we could tell CURL not to validate the peer at all (command line `-k` or [`CURLOPT_SSL_VERIFYPEER`](https://curl.se/libcurl/c/CURLOPT_SSL_VERIFYPEER.html)), this disables all security features of SSH/SFTP. For TLS-based protocols (such as FTP-S, not to be confused with S-FTP!), we could simply tell CURL where the CA cert is inside our request using [CURLOPT_CAINFO](https://curl.se/libcurl/c/CURLOPT_CAINFO.html). However SFTP is ssh-based, not TLS, and uses its own cryptographic library where CA certs are supplied in host files. So we solve this by adding the CA cert(s) to SSH's `known_hosts` file. To make it systemwide, we can put the cert into `/etc/ssh/ssh_known_hosts`.  Read more about SSL certificate verification in CURL [here](https://curl.se/docs/sslcerts.html).

How to use openssl to verify you have the right signing cert:

```
lilhoser@omicrontheta:~/trunk-build$ openssl verify -CAfile ca-cert.pem server-cert.pem

server-cert.pem: OK
```

How to use openssl to sniff SSL handshake:

```
openssl s_client -connect <address>:<port> -showcerts
```
