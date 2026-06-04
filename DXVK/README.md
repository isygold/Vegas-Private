 
 ---
 ### THIS IS THE OFFICIAL BUILD OF STAR ENGINE RECENTLY REBRANDED TO **VEGAS** RECENT BULDS ABOVE V2.7.3.
 * This is the major build structure for Windows.
 
 ---
 ### **FOLDERS**{
* **StarEngine_Release:** 
 (this is where the VEGAS DLL BUILDS is exported)
				  
* **patches:** 
(contains the patch of modified dxvk version)
				  
* **ndk:** 
(contains the ndk sdk for building the project)
				  
* **dxvk-source:** 
(the dxvk file to be patched or modified lives here. note that for every dxvk project pulled or downloaded, 
rename to ```dxvk``` and place them in this directory)
				  
* **build-script:**
(contains custom tuned build commands tailoered for windows){
				                      
* *build_android.sh:* (this is the original build config for the project)
 * *optional build script(DON'T TOUCH UNLESS IMPORTANT)* [
     * *apply_star_logic.sh:* (this is star engine code patch for the dxvk project, also remember to 
                               rename the ```PATH_FILE``` if the path is different or the patch name is different)
* *build_android(Clang).bat:* (this is incomplete for the **starengine code base** but contains complete build
		                       command tailored for ANDROID build **natively**)
* *build_star_engine.sh:* (this is the command build for dxvk when using msys2, especially for the **UCRT64**.)
]
																						
    } *(do note that the 32 bit dll command build isn't made yet)*	 
 }

 ---

 ### These are the files that were edited, look into them for star engine codes;;
* **src/dxgi/dxgi_swapchain.cpp**
* **src/dxvk/dxvk_context.cpp**
* **src/dxvk/dxvk_context.h**
* **src/dxvk/dxvk_device.cpp**
* **src/dxvk/dxvk_graphics.cpp**
* **src/dxvk/dxvk_graphics.h**
* **src/dxvk/dxvk_options.cpp**
* **src/dxvk/dxvk_options.h**
* **src/util/config/config.cpp**
* **src/util/config/config.h**
* *Alternatively you can check the patch created for the codes instantly*

### These are the detailed list of what has been implemented;

| File                                      | Level of Changes     | What is Implemented |
|-------------------------------------------|----------------------|---------------------|
| src/dxgi/dxgi_swapchain.cpp              | Medium-High         | HAAE Tiered Adaptive Scaling logic in Present1(), image surface handling, resolution comparison, and hardware blit decisions |
| src/dxvk/dxvk_context.cpp                | Very Heavy (Main)   | Mid-frame draw threshold flushing, dynamic bind-skip logic, StarEngine profile init, Android logging, HUD version override, draw counter |
| src/dxvk/dxvk_context.h                  | Medium              | Added StarProfile struct, new member variables (m_drawsSinceSubmit, m_starProfile), and function declarations |
| src/dxvk/dxvk_device.cpp                 | Light-Medium        | Device-level initialization and Star Engine profile setup |
| src/dxvk/dxvk_graphics.cpp               | Medium              | Graphics pipeline handling related to bind-skip and dynamic state optimizations |
| src/dxvk/dxvk_graphics.h                 | Light               | Header definitions for graphics-related Star Engine features |
| src/dxvk/dxvk_options.cpp                | Medium              | Registration of new starengine.* config options |
| src/dxvk/dxvk_options.h                  | Light               | Option enum/struct definitions for the new Star Engine config keys |
| src/util/config/config.cpp               | Medium              | Extended to parse the new starengine.* options |
| src/util/config/config.h                 | Light-Medium        | Header support for the new config options |
 ---
 
>CREDITS: 
>DOITSUJIN (MAINTAINER AND FOUNDER OF DXVK),
> ISYGOLD (LEAD DEV FOR STAR ENGINE DXVK AND VEGAS,
>GEMINI (ASSISTANT IN COMPILATION BUILDS AND AGGRESSIVE ERRORS FIXES AND HELP WITH THE ADVANCED CODING).
---

* **COLLABORATORS:**
JACOJJAY, TANAKORN(DEV OF FROST EMULATOR)
