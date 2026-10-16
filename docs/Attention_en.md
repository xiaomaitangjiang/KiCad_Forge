# Important Notes

简体中文 ~ English

## Plugin System

1. Plugins run as independent Python processes with the same filesystem access as the host application. **KiCad Forge cannot provide any security guarantee.** Only install plugins from trusted sources — a malicious plugin can read, modify, or delete arbitrary files.

2. Plugins **should not** rely on side effects of the application itself or its built-in APIs to implement their functionality. For plugins implemented that way, developers do not guarantee backward compatibility.

## About the Project

1. The developer will readily admit that their skills have limits: this project was developed with heavy use of AI-generated code. For any bugs and the inconvenience they may cause you, I sincerely apologize.

2. If you run into a bug, please open an Issue with a detailed description and the application's log file so the bug can be reproduced. If possible, refer to [CONTRIBUTING.md](../CONTRIBUTING.md) and submit a PR to contribute code to this project. Thank you!

3. Please **do not** submit "wishful" issues, for example:

> "The software has a bug"
>
> "Why won't it load anything"
>
> "I can't download the installer"
>
> "Why is there no HarmonyOS support"
