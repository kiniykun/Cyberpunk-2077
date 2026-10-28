This is a new configuration directory.
We should slowly migrate stuff from old r4config/r6config into this one.

The new config layout consist of 4 layers:
 - engine config (read only, platform independent stuff)
 - platform config (read only, platform specyfic stuff only)
 - game config (read only, game specyfic stuff - gameplay settings should go here)
 - user config (read/write)

Directory stucture:
 config\base\*.ini
 config\platform\pc\*.ini
 config\platform\xbox1\*.ini
 config\platform\ps4\*.ini
 config\r4game\*.ini
 config\r6game\*.ini


The user config is ALWAYS stored in the user space (i.e MyDocuments on Windows, user profile on consoles).
The user config should never ever be submitted to P4.

Regarding the PerformancePlatforms:
 - we are moving away from them and trying to incorporate better system
 - a new system was written to handle the Low/Medium/High/Uber settings on PC
 - on consoles we should use platform specyfic config files as there are no Low/Medium/High settings

For PerformancePlatform related stuff, see the "Configuration Matrix" on Wiki (may be WIP when you read this words).

Also, big no no: do not store important engine data in the config files. Game/editor should NEVER crash - even when run without config files. For storing important pesudo config that you don't want to hardcode use CSV/XMLs and put them in the depot (r4data, r6data) folder.

PPL responsible for configs:
 - Tomasz Jonarski (Dex) - engine side
 - Adam Dutkiewicz - Low/Medium/High config

Legacy config files from r4config are put here:
 config\r4game\legacy\*.ini
 config\r6game\legacy\*.ini

Thanks.