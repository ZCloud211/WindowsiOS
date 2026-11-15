// SPDX-License-Identifier: MIT
// One-shot Windows display setup. No driver replacement or resident service.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class WinIOSDisplayTopology
{
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    public struct DisplayDevice {
        public uint cb;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string name;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string description;
        public uint flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string id;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string key;
    }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    public struct Mode {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string device;
        public ushort specVersion, driverVersion, size, driverExtra;
        public uint fields;
        public int x, y;
        public uint orientation, fixedOutput;
        public short color, duplex, yResolution, ttOption, collate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string form;
        public ushort logPixels;
        public uint bits, width, height, displayFlags, frequency;
        public uint icmMethod, icmIntent, mediaType, ditherType, reserved1, reserved2, panWidth, panHeight;
    }
    public sealed class Screen {
        public string name, description, id;
        public bool primary;
        public Mode mode;
    }
    const uint Attached=1, Primary=4, Mirroring=8;
    const uint Position=0x20, Width=0x80000, Height=0x100000;
    const uint UpdateRegistry=1, SetPrimary=0x10, NoReset=0x10000000;
    [DllImport("user32.dll", CharSet=CharSet.Unicode, ExactSpelling=true)]
    [return:MarshalAs(UnmanagedType.Bool)]
    static extern bool EnumDisplayDevicesW(string device, uint index, ref DisplayDevice data, uint flags);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, ExactSpelling=true)]
    [return:MarshalAs(UnmanagedType.Bool)]
    static extern bool EnumDisplaySettingsExW(string device, int index, ref Mode mode, uint flags);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, ExactSpelling=true)]
    static extern int ChangeDisplaySettingsExW(string device, ref Mode mode, IntPtr hwnd, uint flags, IntPtr data);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, ExactSpelling=true, EntryPoint="ChangeDisplaySettingsExW")]
    static extern int Commit(string device, IntPtr mode, IntPtr hwnd, uint flags, IntPtr data);
    [DllImport("user32.dll")]
    [return:MarshalAs(UnmanagedType.Bool)]
    static extern bool SetCursorPos(int x, int y);

    public static Screen[] Read() {
        var screens=new List<Screen>();
        for(uint i=0; i<64; i++) {
            var d=new DisplayDevice(); d.cb=(uint)Marshal.SizeOf(typeof(DisplayDevice));
            if(!EnumDisplayDevicesW(null,i,ref d,0)) break;
            if((d.flags&Attached)==0 || (d.flags&Mirroring)!=0) continue;
            var m=new Mode();m.size=(ushort)Marshal.SizeOf(typeof(Mode));
            if(!EnumDisplaySettingsExW(d.name,-1,ref m,0) || m.width==0 || m.height==0)
                throw new InvalidOperationException("Cannot read active display mode: "+d.name);
            screens.Add(new Screen {name=d.name,description=d.description,id=d.id,primary=(d.flags&Primary)!=0,mode=m});
        }
        return screens.ToArray();
    }
    public static bool IsVirtio(Screen screen) {
        return screen.id!=null && screen.id.StartsWith("PCI\\VEN_1AF4&DEV_1050&",StringComparison.OrdinalIgnoreCase);
    }
    public static Screen Select(Screen[] screens) {
        Screen selected=null;
        foreach(var s in screens) if(IsVirtio(s)) {
            if(selected!=null) throw new InvalidOperationException("More than one active VirtIO display; layout not changed.");
            selected=s;
        }
        if(selected==null) throw new InvalidOperationException("No active PCI VirtIO GPU display found; layout not changed.");
        if(selected.mode.width==0 || selected.mode.height==0 || selected.mode.width>8192 || selected.mode.height>8192)
            throw new InvalidOperationException("Invalid VirtIO display mode; layout not changed.");
        return selected;
    }
    static void Stage(Screen screen,bool primary,bool detach,bool restore) {
        Mode mode=screen.mode;
        mode.size=(ushort)Marshal.SizeOf(typeof(Mode));mode.driverExtra=0;
        // Keep the existing resolution; only detach explicitly selected other
        // paths. Never select a screen by DISPLAY1/2 or by its friendly name.
        mode.fields=Position|Width|Height;
        if(!restore) { mode.x=0; mode.y=0; }
        if(detach) { mode.width=0; mode.height=0; }
        int code=ChangeDisplaySettingsExW(screen.name,ref mode,IntPtr.Zero,
            UpdateRegistry|NoReset|(primary?SetPrimary:0),IntPtr.Zero);
        if(code!=0) throw new InvalidOperationException("Display staging failed: "+screen.name+", code "+code);
    }
    static void ApplyStaged() {
        int code=Commit(null,IntPtr.Zero,IntPtr.Zero,0,IntPtr.Zero);
        if(code!=0) throw new InvalidOperationException("Display configuration apply failed: "+code);
    }
    public static void ApplyVirtioOnly(Screen[] before) {
        Screen target=Select(before);
        Stage(target,true,false,false);
        foreach(var s in before) if(s.name!=target.name) Stage(s,false,true,false);
        ApplyStaged();
        Screen[] after=Read();Screen current=Select(after);
        if(after.Length!=1 || !current.primary || current.mode.x!=0 || current.mode.y!=0 || current.name!=target.name)
            throw new InvalidOperationException("VirtIO-only layout readback failed.");
        SetCursorPos((int)current.mode.width/2,(int)current.mode.height/2);
    }
    public static void Restore(Screen[] before) {
        if(before.Length==0) throw new InvalidOperationException("No original layout available.");
        foreach(var s in before) if(s.primary) Stage(s,true,false,true);
        foreach(var s in before) if(!s.primary) Stage(s,false,false,true);
        ApplyStaged();
    }
    public static void SelfTest() {
        if(Marshal.SizeOf(typeof(Mode))!=220 || Marshal.SizeOf(typeof(DisplayDevice))!=840)
            throw new Exception("Win32 display ABI size mismatch.");
        var gpu=new Screen {name="\\\\.\\DISPLAY1",id="PCI\\VEN_1AF4&DEV_1050&SUBSYS_11001AF4",mode=new Mode {width=1600,height=1200}};
        var other=new Screen {name="\\\\.\\DISPLAY2",description="Red Hat VirtIO (name alone must not match)",id="ROOT\\BASICDISPLAY",primary=true,mode=new Mode {width=800,height=600}};
        if(Select(new[]{other,gpu})!=gpu) throw new Exception("Selected wrong adapter.");
        bool rejected=false;try {Select(new[]{other});} catch(InvalidOperationException) {rejected=true;}
        if(!rejected) throw new Exception("Missing VirtIO device accepted.");
        rejected=false;try {Select(new[]{gpu,gpu});} catch(InvalidOperationException) {rejected=true;}
        if(!rejected) throw new Exception("Ambiguous VirtIO device accepted.");
    }
}
