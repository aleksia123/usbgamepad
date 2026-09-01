using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace AppTransportTest;

/// <summary>
/// Minimal raw HID transport (no NuGet dependencies - direct P/Invoke against
/// hid.dll / setupapi.dll), just enough to find the RP2350 app-transport
/// interface by VID/PID and exchange the 13-byte (report ID + 12-byte
/// payload) reports described in src/app_transport.h.
/// </summary>
internal sealed class HidDevice : IDisposable
{
    private const int DIGCF_PRESENT = 0x02;
    private const int DIGCF_DEVICEINTERFACE = 0x10;
    private const uint GENERIC_READ = 0x80000000;
    private const uint GENERIC_WRITE = 0x40000000;
    private const uint FILE_SHARE_READ = 0x1;
    private const uint FILE_SHARE_WRITE = 0x2;
    private const uint OPEN_EXISTING = 3;
    // Without overlapped I/O, a blocking Read on the background reader thread
    // serializes the single device handle and starves WriteReport - only the
    // very first output report gets through. Overlapped + async lets the IN and
    // OUT pipes run concurrently.
    private const uint FILE_FLAG_OVERLAPPED = 0x40000000;

    [StructLayout(LayoutKind.Sequential)]
    private struct SP_DEVICE_INTERFACE_DATA
    {
        public int cbSize;
        public Guid InterfaceClassGuid;
        public int Flags;
        public IntPtr Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct HIDD_ATTRIBUTES
    {
        public int Size;
        public ushort VendorID;
        public ushort ProductID;
        public ushort VersionNumber;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct HIDP_CAPS
    {
        public ushort Usage;
        public ushort UsagePage;
        public ushort InputReportByteLength;
        public ushort OutputReportByteLength;
        public ushort FeatureReportByteLength;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 17)]
        public ushort[] Reserved;
        public ushort NumberLinkCollectionNodes;
        public ushort NumberInputButtonCaps;
        public ushort NumberInputValueCaps;
        public ushort NumberInputDataIndices;
        public ushort NumberOutputButtonCaps;
        public ushort NumberOutputValueCaps;
        public ushort NumberOutputDataIndices;
        public ushort NumberFeatureButtonCaps;
        public ushort NumberFeatureValueCaps;
        public ushort NumberFeatureDataIndices;
    }

    [DllImport("hid.dll")] private static extern void HidD_GetHidGuid(out Guid guid);
    [DllImport("hid.dll")] private static extern bool HidD_GetAttributes(SafeFileHandle handle, out HIDD_ATTRIBUTES attributes);
    [DllImport("hid.dll")] private static extern bool HidD_GetPreparsedData(SafeFileHandle handle, out IntPtr preparsedData);
    [DllImport("hid.dll")] private static extern bool HidD_FreePreparsedData(IntPtr preparsedData);
    [DllImport("hid.dll")] private static extern int HidP_GetCaps(IntPtr preparsedData, out HIDP_CAPS caps);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern IntPtr SetupDiGetClassDevs(ref Guid classGuid, IntPtr enumerator, IntPtr hwndParent, int flags);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiEnumDeviceInterfaces(IntPtr deviceInfoSet, IntPtr deviceInfoData, ref Guid interfaceClassGuid, uint memberIndex, ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData);

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Auto)]
    private static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr deviceInfoSet, ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData, IntPtr deviceInterfaceDetailData, uint deviceInterfaceDetailDataSize, out uint requiredSize, IntPtr deviceInfoData);

    [DllImport("setupapi.dll")] private static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceInfoSet);

    [DllImport("kernel32.dll", CharSet = CharSet.Auto, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(string fileName, uint desiredAccess, uint shareMode, IntPtr securityAttributes, uint creationDisposition, uint flagsAndAttributes, IntPtr templateFile);

    public int InputReportLength { get; }
    public int OutputReportLength { get; }

    private readonly FileStream _stream;

    private HidDevice(SafeFileHandle handle, int inputLen, int outputLen)
    {
        InputReportLength = inputLen;
        OutputReportLength = outputLen;
        _stream = new FileStream(handle, FileAccess.ReadWrite, bufferSize: 1, isAsync: true);
    }

    /// <summary>Finds and opens the first HID device matching vid/pid.</summary>
    public static HidDevice? FindAndOpen(ushort vid, ushort pid)
    {
        HidD_GetHidGuid(out Guid hidGuid);

        IntPtr deviceInfoSet = SetupDiGetClassDevs(ref hidGuid, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (deviceInfoSet == IntPtr.Zero || deviceInfoSet.ToInt64() == -1) return null;

        try
        {
            uint index = 0;
            while (true)
            {
                var ifData = new SP_DEVICE_INTERFACE_DATA();
                ifData.cbSize = Marshal.SizeOf<SP_DEVICE_INTERFACE_DATA>();

                if (!SetupDiEnumDeviceInterfaces(deviceInfoSet, IntPtr.Zero, ref hidGuid, index, ref ifData))
                    break; // no more devices

                index++;

                SetupDiGetDeviceInterfaceDetail(deviceInfoSet, ref ifData, IntPtr.Zero, 0, out uint requiredSize, IntPtr.Zero);
                if (requiredSize == 0) continue;

                IntPtr detailBuffer = Marshal.AllocHGlobal((int)requiredSize);
                try
                {
                    // cbSize of SP_DEVICE_INTERFACE_DETAIL_DATA is 6 on 64-bit (int + char marker), write it manually.
                    Marshal.WriteInt32(detailBuffer, IntPtr.Size == 8 ? 8 : 6);

                    if (!SetupDiGetDeviceInterfaceDetail(deviceInfoSet, ref ifData, detailBuffer, requiredSize, out _, IntPtr.Zero))
                        continue;

                    string devicePath = Marshal.PtrToStringAuto(detailBuffer + 4) ?? "";
                    if (string.IsNullOrEmpty(devicePath)) continue;

                    SafeFileHandle handle = CreateFile(devicePath, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, IntPtr.Zero);

                    if (handle.IsInvalid) continue;

                    if (!HidD_GetAttributes(handle, out HIDD_ATTRIBUTES attrs) ||
                        attrs.VendorID != vid || attrs.ProductID != pid)
                    {
                        handle.Dispose();
                        continue;
                    }

                    if (!HidD_GetPreparsedData(handle, out IntPtr preparsed))
                    {
                        handle.Dispose();
                        continue;
                    }

                    HidP_GetCaps(preparsed, out HIDP_CAPS caps);
                    HidD_FreePreparsedData(preparsed);

                    return new HidDevice(handle, caps.InputReportByteLength, caps.OutputReportByteLength);
                }
                finally
                {
                    Marshal.FreeHGlobal(detailBuffer);
                }
            }
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(deviceInfoSet);
        }

        return null;
    }

    public byte[]? ReadReport()
    {
        var buffer = new byte[InputReportLength];
        try
        {
            // Overlapped async read: does not hold the handle against writes.
            int read = _stream.ReadAsync(buffer, 0, buffer.Length).GetAwaiter().GetResult();
            return read == buffer.Length ? buffer : null;
        }
        catch (IOException)
        {
            return null;
        }
    }

    public bool WriteReport(byte[] report)
    {
        try
        {
            // Overlapped async write: runs concurrently with a pending read.
            _stream.WriteAsync(report, 0, report.Length).GetAwaiter().GetResult();
            return true;
        }
        catch (IOException)
        {
            return false;
        }
    }

    public void Dispose() => _stream.Dispose();
}
