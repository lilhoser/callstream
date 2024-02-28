# Introduction

`callstream` is a multi-threaded plugin for [trunk-recorder](https://github.com/robotastic/trunk-recorder). This plugin streams complete audio calls recorded by trunk-recorder from conventional and trunked radio systems, such as local fire/rescue/EMS.

This plugin was designed to work with [pizzawave](https://github.com/lilhoser/pizzawave), a .NET application that transcribes the recorded WAV files to text using [OpenAI's Whisper AI model](https://openai.com/research/whisper) as exposed through [whisper.net toolchain](https://github.com/sandrohanea/whisper.net).

# Requirements
* Linux system running trunk-recorder

# Installation

* Copy the contents of this repo to `trunk-recorder/plugins/callstream`
* Update `trunk-recorder/CMakeLists.txt`:
```
add_subdirectory(plugins/callstream)
```
* [Rebuild trunk-recorder](https://trunkrecorder.com/docs/Install/INSTALL-LINUX)

# Configure

Add the following code to your trunk-recorder's JSON configuration:

```
"plugins": [
        {
            "name":"callstream",
            "library":"libcallstream.so",
            "address":"<ip address of server>",
            "port":<port of server>,
            "streams":[
                {
                    "TGID":0,
                    "shortName":"<system name>"
                }
            ]
        }
    ]
```

Make sure you set the global `audioStreaming` to `true`.

* `Ip address of server`: this is the IP address of the machine that will receive the streamed calls. The `callstream` plugin acts like a client, transmitting call data to a remote server. The plugin was designed with [pizzawave](https://github.com/lilhoser/pizzawave) being the server, but you can easily write your own server and do whatever you want to with the call data.
* `Port of server`: the port your server is listening on; this is configurable in pizzawave
* `System name`: this is the name of the trunk-recorder system from your configuration file.

If TGID is set to 0, all calls from all talkgroups will be sent. If TGID is set to a specific decimal value, only calls from that talkgroup will be sent.

You might consider disabling these [per-system settings](https://trunkrecorder.com/docs/CONFIGURE) in trunk-recorder, if you're not using them for another purpose:
* `audioArchive` - this setting controls whether or not trunk-recorder writes WAV files to disk after it processes calls. Since `callstream` sends the same data over the wire to your server, this is wasted processing.
* `compressWav` - this setting controls whether WAVs are compressed before writing them to disk (something needed for other plugins like openmhz), which is unnecessary if `audioArchive` is disabled
* `transmissionArchive` - this setting should always be disabled unless you are performing low-level diagnostics

# Run

Once you have rebuilt trunk-recorder with the plugin code and modified your trunk-recorder config to enable the plugin, you can run trunk-recorder to begin transmitting calls. In the console, you should see log messages prefixed with `callstream`. If you need to see more detailed diagnostics, bump up your log level in trunk-recorder config file's global `logLevel` setting.

# Data Protocol

If you're writing your own server to consume call data sent by this plugin, you will need to read the data as follows:

| Offset      | Length (bytes) |  Description            |
| ----------- | -------------- | ----------------------- |
| 0           | 4              | Magic 'PZZA'            |
| 4           | 8              | JSON length in bytes    |
| 12          | 4              | Number of audio samples |
| 16          | (variable)     | JSON data               |
| (variable)  | (variable)     | Sample data, int16 ea.  |

This data is guaranteed to be sent in this order.  Note at the time of writing, that the sample data is recorded by trunk-recorder as 16-bit, 1 channel, 8khz sampling rate.

JSON structure:

| Field Name             | Length (bytes) |  Description            |
| ---------------------- | -------------- | ----------------------- |
| Source                 | 4              | Recorder source         |
| Talkgroup              | 8              | Talkgroup ID            |
| PatchedTalkgroups      | (variable)     | array of patched TGs    |
| Frequency              | (variable)     | The call frequency      |
| SystemShortName        | (variable)     | The name of the system  |
| CallId                 | 4              | The ID of the call      |
| StartTime              | 8              | Unix seconds            |
| StopTime               | 8              | Unix seconds            |


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