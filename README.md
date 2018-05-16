# ART - The Android Runtime (Fork for Project ARTist)

[![Gitter](https://badges.gitter.im/Project-ARTist/meta.svg)](https://gitter.im/project-artist/Lobby?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge&utm_content=body_badge)

This repository is a fork of AOSP's ART project, Androids default runtime since Lollipop (5.0), which integrates the [ARTist](https://github.com/Project-ARTist/ARTist) instrumentation framework into ART's ```dex2oat``` on-device compiler to allow for instrumentation of apps and middleware components during compilation. For more information about ARTist, see the corresponding section below. 

We try to keep the integration code as small as possible to ease the process of adding support for new versions and stay robust to compiler changes. Still, as ART is an ever-evolving project, we need to maintain at least one branch per version. 

Currently, we support the following Android versions:

|Name|Version|Branch|
|---|---|---|
|Marshmallow|6.0|```artist_marshmallow_master```|
|Nougat|7.0|```artist_nougat_master```|
|Nougat|7.1|```artist_nougat_7.1_master```|
|Oreo (experimental)|8.0|```artist_oreo_master```|


## Project Structure

The [ARTist repository](https://github.com/Project-ARTist/ARTist) is added as a submodule under ```compiler/optimizing/artist``` and provides the needed code for the integration. Because this repository takes care of the differences between Android versions, the ARTist codebase can stay mostly version-independent. 

Currently, there is only a small number of places where we added code to ART. Here is a quick primer at the two files where our major changes reside:

- ```dex2oat.cc``` : ARTist and its modules are initialized early on. In the future, the modules will not be hardcoded at compile-time but be loaded dynamically at runtime (see beta section below)
- ```optimizing_compiler.cc```: For each single method in the current compilation target (app, framework, system server), a set of optimizations are executed. We disguise our instrumentation modules as optimization passes so in this file we are just adding them to the list so the optimization framework takes care of executing them.

In fact we are only modifying code that relates to the compiler but *no* internals of the actual runtime environment, since in the end we deploy ARTist with the compiler binary but *never* change other parts of the runtime on the device (e.g., the ```art``` binary). The deployment options described in the ARTist section below give an intuition why this is important. 

## A few words on ```dex2oat```

Android's novel on-device compiler is used to compile apps and system libraries from dalvik bytecode to the platform's native code. While it supports multiple backends, we opted to utilize the so-called ```Optimizing``` backend that has a single intermediate representation (IR) and is  deemed default since Android 6. The fact that our *optimizations*, which are actually instrumentation modules, are operating on the IR gives us support for all hardware architectures that are officially supported by Android for free, since the native code generators stay untouched and therefore work as usual. 


# ARTist - The Android Runtime Instrumentation and Security Toolkit

ARTist is a flexible open source instrumentation framework for Android's apps and Java middleware. It is based on the Android Runtime’s (ART) compiler and modifies code during on-device compilation. In contrast to existing instrumentation frameworks, it preserves the application's original signature and operates on the instruction level. 

This repository is the glue that keeps together the ```dex2oat``` compiler and our [ARTist](https://github.com/Project-ARTist/ARTist) extensions and ensures compatibility for different Android versions. 

ARTist can be deployed in two different ways: First, as a regular application using our [ArtistGui](https://github.com/Project-ARTist/ArtistGui) project that allows for non-invasive app instrumentation on rooted devices, or second, as a system compiler for custom ROMs where it can additionally instrument the system server (Package Manager Service, Activity Manager Service, ...) and the Android framework classes (```boot.oat```). It supports Android versions after (and including) Marshmallow 6.0. 

For detailed tutorials and more in-depth information on the ARTist ecosystem, have a look at our [official documentation](https://artist.cispa.saarland) and join our [Gitter chat](https://gitter.im/project-artist/Lobby).

## Upcoming Beta Release

We are about to enter the beta phase soon, which will bring a lot of changes to the whole ARTist ecosystem, including a dedicated ARTist SDK for simplified Module development, a semantic versioning-inspired release and versioning scheme, an improved and updated version of our online documentation, great new Modules, and a lot more improvements. However, in particular during the transition phase, some information like the one in the repositories' README.md files and the documentation at [https://artist.cispa.saarland](https://artist.cispa.saarland) might be slightly out of sync. We apologize for the inconvenience and happily take feedback at [Gitter](https://gitter.im/project-artist/Lobby). To keep up with the current progress, keep an eye on the beta milestones of the Project: ARTist repositories and check for new blog posts at [https://artist.cispa.saarland](https://artist.cispa.saarland) . 

## Contribution

We hope to create an active community of developers, researchers and users around Project ARTist and hence are happy about contributions and feedback of any kind. There are plenty of ways to get involved and help the project, such as testing and writing Modules, providing feedback on which functionality is key or missing, reporting bugs and other issues, or in general talk about your experiences. The team is actively monitoring [Gitter](https://gitter.im/project-artist/) and of course the repositories, and we are happy to get in touch and discuss. We do not have a full-fledged contribution guide, yet, but it will follow soon (see beta announcement above). 

## Academia

ARTist is based on a paper called **ARTist - The Android Runtime Instrumentation and Security Toolkit**, published at the 2nd IEEE European Symposium on Security and Privacy (EuroS&P'17). The full paper is available [here](https://artist.cispa.saarland/res/papers/ARTist.pdf). If you are citing ARTist in your research, please use the following bibliography entry:

```
@inproceedings{artist,
  title={ARTist: The Android runtime instrumentation and security toolkit},
  author={Backes, Michael and Bugiel, Sven and Schranz, Oliver and von Styp-Rekowsky, Philipp and Weisgerber, Sebastian},
  booktitle={2017 IEEE European Symposium on Security and Privacy (EuroS\&P)},
  pages={481--495},
  year={2017},
  organization={IEEE}
}
```

There is a follow-up paper where we utilized ARTist to cut out advertisement libraries from third-party applications, move the library to a dedicated app (own security principal) and reconnect both using a custom Binder IPC protocol, all while preserving visual fidelity by displaying the remote advertisements as floating views on top of the now ad-cleaned application. The full paper **The ART of App Compartmentalization: Compiler-based Library Privilege Separation on Stock Android**, as it was published at the 2017 ACM SIGSAC Conference on Computer and Communications Security (CCS'17), is available [here](https://artist.cispa.saarland/res/papers/CompARTist.pdf).
