// 便携版自解压启动器（编译为 winexe，无控制台窗口）。
//
// 结构： [本启动器 exe][zip 负载][16 字节尾注: int64 负载偏移, int64 负载长度]
// 行为： 首次双击 -> 解压到自身所在目录下的 BadmintonAnalyzer\ 并启动应用；
//        再次双击 -> 直接启动已解压的应用（不重复解压）。
//
// 用 csc（.NET Framework 4.8，Windows 自带）编译，所以最终 exe 不需要用户装任何运行时。

using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Threading;
using System.Windows.Forms;

internal static class Program
{
    private const string TargetFolderName = "BadmintonAnalyzer";
    private const string EntryExecutable = "badminton-analyzer.exe";

    [STAThread]
    private static int Main(string[] args)
    {
        Application.EnableVisualStyles();
        try
        {
            string self = Assembly.GetExecutingAssembly().Location;
            if (string.IsNullOrEmpty(self))
                self = Process.GetCurrentProcess().MainModule.FileName;
            string baseDirectory = Path.GetDirectoryName(self);
            string target = Path.Combine(baseDirectory, TargetFolderName);

            string entry = FindEntry(target);
            if (entry == null)
            {
                using (var progress = new ProgressForm())
                {
                    progress.Show();
                    progress.Refresh();
                    Thread worker = new Thread(delegate()
                    {
                        try
                        {
                            Extract(self, target, progress);
                            progress.Finished = true;
                        }
                        catch (Exception error)
                        {
                            progress.Error = error;
                            progress.Finished = true;
                        }
                    });
                    worker.IsBackground = true;
                    worker.Start();
                    while (!progress.Finished)
                    {
                        Application.DoEvents();
                        Thread.Sleep(30);
                    }
                    if (progress.Error != null)
                        throw progress.Error;
                }
                entry = FindEntry(target);
            }

            if (entry == null)
                throw new FileNotFoundException("解压后仍找不到 " + EntryExecutable);

            Process.Start(new ProcessStartInfo(entry)
            {
                WorkingDirectory = Path.GetDirectoryName(entry),
                UseShellExecute = true
            });
            return 0;
        }
        catch (Exception error)
        {
            MessageBox.Show("启动失败：" + error.Message, "羽毛球回合分析器",
                            MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }

    private static string FindEntry(string target)
    {
        if (!Directory.Exists(target))
            return null;
        string direct = Path.Combine(target, EntryExecutable);
        if (File.Exists(direct))
            return direct;
        string[] found = Directory.GetFiles(target, EntryExecutable, SearchOption.AllDirectories);
        return found.Length > 0 ? found[0] : null;
    }

    private static void Extract(string self, string target, ProgressForm progress)
    {
        using (FileStream file = File.OpenRead(self))
        {
            long offset;
            long length;
            ReadTrailer(file, out offset, out length);
            using (var payload = new SubStream(file, offset, length))
            using (var archive = new ZipArchive(payload, ZipArchiveMode.Read))
            {
                Directory.CreateDirectory(target);
                string root = Path.GetFullPath(target);
                int index = 0;
                foreach (ZipArchiveEntry item in archive.Entries)
                {
                    index++;
                    progress.Report(index * 100 / Math.Max(1, archive.Entries.Count));
                    if (string.IsNullOrEmpty(item.Name))
                        continue;  // 目录项
                    string destination = Path.GetFullPath(Path.Combine(target, item.FullName));
                    if (!destination.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                        continue;  // 防目录穿越
                    Directory.CreateDirectory(Path.GetDirectoryName(destination));
                    item.ExtractToFile(destination, true);
                }
            }
        }
    }

    // 末尾 16 字节：int64 偏移 + int64 长度（打包脚本写入）
    private static void ReadTrailer(FileStream file, out long offset, out long length)
    {
        byte[] trailer = new byte[16];
        file.Seek(-trailer.Length, SeekOrigin.End);
        int read = 0;
        while (read < trailer.Length)
        {
            int count = file.Read(trailer, read, trailer.Length - read);
            if (count <= 0)
                break;
            read += count;
        }
        if (read != trailer.Length)
            throw new InvalidDataException("文件尾部缺少负载信息，可能不是完整的便携版");
        offset = BitConverter.ToInt64(trailer, 0);
        length = BitConverter.ToInt64(trailer, 8);
        if (offset <= 0 || length <= 0 || offset + length > file.Length)
            throw new InvalidDataException("负载信息无效（文件可能被截断）");
        file.Seek(offset, SeekOrigin.Begin);
    }

    private sealed class SubStream : Stream
    {
        private readonly Stream inner;
        private readonly long start;
        private readonly long length;
        private long position;

        public SubStream(Stream inner, long start, long length)
        {
            this.inner = inner;
            this.start = start;
            this.length = length;
        }

        public override bool CanRead { get { return true; } }
        public override bool CanSeek { get { return true; } }
        public override bool CanWrite { get { return false; } }
        public override long Length { get { return length; } }

        public override long Position
        {
            get { return position; }
            set { position = value; }
        }

        public override int Read(byte[] buffer, int offset, int count)
        {
            if (position >= length)
                return 0;
            long remaining = length - position;
            int wanted = (int)Math.Min(count, remaining);
            inner.Seek(start + position, SeekOrigin.Begin);
            int read = inner.Read(buffer, offset, wanted);
            position += read;
            return read;
        }

        public override long Seek(long offset, SeekOrigin origin)
        {
            long next = origin == SeekOrigin.Begin ? offset
                      : origin == SeekOrigin.Current ? position + offset
                      : length + offset;
            position = Math.Max(0, Math.Min(length, next));
            return position;
        }

        public override void Flush() { }
        public override void SetLength(long value) { throw new NotSupportedException(); }
        public override void Write(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
    }

    private sealed class ProgressForm : Form
    {
        private readonly Label label;
        private readonly ProgressBar bar;

        public volatile bool Finished;
        public volatile Exception Error;

        public ProgressForm()
        {
            Text = "羽毛球回合分析器 · 正在解压";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterScreen;
            MaximizeBox = false;
            MinimizeBox = false;
            ClientSize = new Size(400, 96);

            label = new Label();
            label.Text = "首次运行：正在解压便携版文件…";
            label.AutoSize = false;
            label.SetBounds(16, 16, 368, 20);
            Controls.Add(label);

            bar = new ProgressBar();
            bar.SetBounds(16, 44, 368, 20);
            bar.Minimum = 0;
            bar.Maximum = 100;
            Controls.Add(bar);
        }

        public void Report(int percent)
        {
            if (percent < 0)
                percent = 0;
            if (percent > 100)
                percent = 100;
            if (InvokeRequired)
            {
                try { BeginInvoke(new Action<int>(Report), percent); }
                catch (InvalidOperationException) { }
                return;
            }
            bar.Value = percent;
            label.Text = "正在解压便携版文件… " + percent + "%";
        }
    }
}
