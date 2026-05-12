using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows;
using System.Windows.Threading;
using Microsoft.Win32;

namespace MaaInstanceManager;

public partial class MainWindow : INotifyPropertyChanged
{
    private const string ExecutableName = "MAA.exe";
    private const string DefaultConfigurationName = "Default";
    private const string ConnectAddressKey = "Connect.Address";
    private const string ConnectAddressHistoryKey = "Connect.AddressHistory";

    private readonly string _configDirectory = Path.Combine(AppContext.BaseDirectory, "config");
    private readonly string _stateFile;
    private readonly DispatcherTimer _refreshTimer = new() { Interval = TimeSpan.FromSeconds(2) };

    private string _releasePackagePath = string.Empty;
    private string _workspaceRoot = Path.Combine(AppContext.BaseDirectory, "managed_instances");
    private string _instanceNamePrefix = "MAA-";
    private int _newInstanceCount = 1;
    private int _cloneCount = 1;
    private int _startAdbPort = 16384;
    private int _portStep = 32;
    private string _statusMessage = "就绪";
    private ManagedInstance? _selectedInstance;
    private bool _isBusy;

    public MainWindow()
    {
        InitializeComponent();
        _stateFile = Path.Combine(_configDirectory, "instance_manager.json");
        Instances.CollectionChanged += InstancesCollectionChanged;
        LoadState();
        DataContext = this;
        RefreshInstances();

        _refreshTimer.Tick += (_, _) => RefreshRunningStates();
        _refreshTimer.Start();
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public ObservableCollection<ManagedInstance> Instances { get; } = [];

    public string ReleasePackagePath
    {
        get => _releasePackagePath;
        set {
            if (SetField(ref _releasePackagePath, value))
            {
                SaveState();
            }
        }
    }

    public string WorkspaceRoot
    {
        get => _workspaceRoot;
        set {
            if (SetField(ref _workspaceRoot, value))
            {
                SaveState();
            }
        }
    }

    public string InstanceNamePrefix
    {
        get => _instanceNamePrefix;
        set {
            if (SetField(ref _instanceNamePrefix, value))
            {
                SaveState();
            }
        }
    }

    public int NewInstanceCount
    {
        get => _newInstanceCount;
        set {
            if (SetField(ref _newInstanceCount, Math.Clamp(value, 1, 999)))
            {
                SaveState();
            }
        }
    }

    public int CloneCount
    {
        get => _cloneCount;
        set {
            if (SetField(ref _cloneCount, Math.Clamp(value, 1, 999)))
            {
                SaveState();
            }
        }
    }

    public int StartAdbPort
    {
        get => _startAdbPort;
        set {
            if (SetField(ref _startAdbPort, Math.Clamp(value, 1, 65535)))
            {
                SaveState();
            }
        }
    }

    public int PortStep
    {
        get => _portStep;
        set {
            if (SetField(ref _portStep, Math.Clamp(value, 1, 65535)))
            {
                SaveState();
            }
        }
    }

    public string StatusMessage
    {
        get => _statusMessage;
        set => SetField(ref _statusMessage, value);
    }

    public ManagedInstance? SelectedInstance
    {
        get => _selectedInstance;
        set {
            if (SetField(ref _selectedInstance, value))
            {
                NotifySummaryProperties();
            }
        }
    }

    public int TotalCount => Instances.Count;

    public int SelectedCount => Instances.Count(static instance => instance.IsSelected);

    public int RunningCount => Instances.Count(static instance => instance.IsRunning);

    private async void CreateInstancesButton_Click(object sender, RoutedEventArgs e)
    {
        if (!ValidateReleasePackage() || !ValidateWorkspaceRoot())
        {
            return;
        }

        var count = NewInstanceCount;
        await RunBusyAsync($"正在从 release 创建 {count} 个实例...", () => Task.Run(() => {
            var plans = BuildCreationPlan(count);
            var created = new List<ManagedInstance>(plans.Count);
            foreach (var plan in plans)
            {
                try
                {
                    ZipFile.ExtractToDirectory(ReleasePackagePath, plan.DirectoryPath);
                    NormalizeExtractedReleaseRoot(plan.DirectoryPath);
                    EnsureExecutableExists(plan.DirectoryPath);
                    ApplyAdbPort(plan.DirectoryPath, plan.AdbPort);
                    created.Add(CreateInstance(plan.Name, plan.DirectoryPath, plan.AdbPort, "已创建"));
                }
                catch
                {
                    TryDeleteDirectory(plan.DirectoryPath);
                    throw;
                }
            }

            Dispatcher.Invoke(() => {
                foreach (var instance in created)
                {
                    Instances.Add(instance);
                }

                SaveState();
                RefreshInstances();
                StatusMessage = $"已创建 {created.Count} 个实例";
            });
        }));
    }

    private async void CloneSelectedButton_Click(object sender, RoutedEventArgs e)
    {
        if (!ValidateWorkspaceRoot())
        {
            return;
        }

        var source = SelectedInstance;
        if (source is null)
        {
            StatusMessage = "请选择一个已经手动配置好的源实例";
            return;
        }

        RefreshRunningStates();
        if (source.IsRunning)
        {
            StatusMessage = "源实例正在运行，请先关闭后再复制";
            return;
        }

        if (!Directory.Exists(source.DirectoryPath) || !File.Exists(GetExecutablePath(source.DirectoryPath)))
        {
            StatusMessage = "源实例目录无效";
            return;
        }

        var count = CloneCount;
        await RunBusyAsync($"正在复制 {count} 个实例...", () => Task.Run(() => {
            var plans = BuildCreationPlan(count);
            var created = new List<ManagedInstance>(plans.Count);
            foreach (var plan in plans)
            {
                try
                {
                    CopyDirectory(source.DirectoryPath, plan.DirectoryPath);
                    ApplyAdbPort(plan.DirectoryPath, plan.AdbPort);
                    created.Add(CreateInstance(plan.Name, plan.DirectoryPath, plan.AdbPort, "已复制"));
                }
                catch
                {
                    TryDeleteDirectory(plan.DirectoryPath);
                    throw;
                }
            }

            Dispatcher.Invoke(() => {
                foreach (var instance in created)
                {
                    Instances.Add(instance);
                }

                SaveState();
                RefreshInstances();
                StatusMessage = $"已复制 {created.Count} 个实例";
            });
        }));
    }

    private async void RemapPortsButton_Click(object sender, RoutedEventArgs e)
    {
        var targets = GetSelectedOrAllInstances()
            .OrderBy(static instance => instance.Name, StringComparer.OrdinalIgnoreCase)
            .ToList();
        if (targets.Count == 0)
        {
            StatusMessage = "没有可映射的实例";
            return;
        }

        await RunBusyAsync("正在按顺序重新映射 ADB 端口...", () => Task.Run(() => {
            for (var index = 0; index < targets.Count; index++)
            {
                var port = StartAdbPort + (index * PortStep);
                ApplyAdbPort(targets[index].DirectoryPath, port);
                Dispatcher.Invoke(() => {
                    targets[index].AdbPort = port;
                    targets[index].Status = "端口已更新";
                });
            }

            Dispatcher.Invoke(() => {
                SaveState();
                RefreshInstances();
                StatusMessage = $"已映射 {targets.Count} 个实例";
            });
        }));
    }

    private void RefreshButton_Click(object sender, RoutedEventArgs e)
    {
        RefreshInstances();
        StatusMessage = "已刷新";
    }

    private void SelectAllButton_Click(object sender, RoutedEventArgs e)
    {
        foreach (var instance in Instances)
        {
            instance.IsSelected = true;
        }

        NotifySummaryProperties();
    }

    private void ClearSelectionButton_Click(object sender, RoutedEventArgs e)
    {
        foreach (var instance in Instances)
        {
            instance.IsSelected = false;
        }

        NotifySummaryProperties();
    }

    private async void StartSelectedButton_Click(object sender, RoutedEventArgs e)
    {
        await StartInstances(GetSelectedOrFocusedInstances());
    }

    private async void StopSelectedButton_Click(object sender, RoutedEventArgs e)
    {
        await StopInstances(GetSelectedOrFocusedInstances());
    }

    private async void StartAllButton_Click(object sender, RoutedEventArgs e)
    {
        await StartInstances(Instances.ToList());
    }

    private async void StopAllButton_Click(object sender, RoutedEventArgs e)
    {
        await StopInstances(Instances.ToList());
    }

    private void SelectReleasePackageButton_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Filter = "MAA Release (*.zip)|*.zip|所有文件 (*.*)|*.*",
            CheckFileExists = true,
            Multiselect = false,
        };

        if (File.Exists(ReleasePackagePath))
        {
            dialog.InitialDirectory = Path.GetDirectoryName(ReleasePackagePath);
            dialog.FileName = Path.GetFileName(ReleasePackagePath);
        }

        if (dialog.ShowDialog(this) == true)
        {
            ReleasePackagePath = dialog.FileName;
        }
    }

    private void SelectWorkspaceRootButton_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog
        {
            Title = "选择实例总目录",
            InitialDirectory = Directory.Exists(WorkspaceRoot) ? WorkspaceRoot : AppContext.BaseDirectory,
        };

        if (dialog.ShowDialog(this) == true && !string.IsNullOrWhiteSpace(dialog.FolderName))
        {
            WorkspaceRoot = dialog.FolderName;
            RefreshInstances();
        }
    }

    private void OpenSelectedFolderButton_Click(object sender, RoutedEventArgs e)
    {
        var target = SelectedInstance;
        if (target is null || !Directory.Exists(target.DirectoryPath))
        {
            StatusMessage = "请选择一个有效实例";
            return;
        }

        OpenFolder(target.DirectoryPath);
    }

    private void OpenWorkspaceFolderButton_Click(object sender, RoutedEventArgs e)
    {
        Directory.CreateDirectory(WorkspaceRoot);
        OpenFolder(WorkspaceRoot);
    }

    private async Task StartInstances(IReadOnlyCollection<ManagedInstance> targets)
    {
        if (targets.Count == 0)
        {
            StatusMessage = "请选择要启动的实例";
            return;
        }

        await RunBusyAsync("正在启动实例...", () => Task.Run(() => {
            foreach (var instance in targets)
            {
                StartInstance(instance);
            }

            Dispatcher.Invoke(() => {
                RefreshRunningStates();
                StatusMessage = $"已处理 {targets.Count} 个启动请求";
            });
        }));
    }

    private async Task StopInstances(IReadOnlyCollection<ManagedInstance> targets)
    {
        if (targets.Count == 0)
        {
            StatusMessage = "请选择要关闭的实例";
            return;
        }

        await RunBusyAsync("正在关闭实例...", () => Task.Run(() => {
            foreach (var instance in targets)
            {
                StopInstance(instance);
            }

            Dispatcher.Invoke(() => {
                RefreshRunningStates();
                StatusMessage = $"已处理 {targets.Count} 个关闭请求";
            });
        }));
    }

    private void StartInstance(ManagedInstance instance)
    {
        try
        {
            using var existing = FindRunningProcess(instance);
            if (existing is not null)
            {
                Dispatcher.Invoke(() => {
                    instance.ProcessId = existing.Id;
                    instance.IsRunning = true;
                    instance.Status = "已经运行";
                });
                return;
            }

            var executablePath = GetExecutablePath(instance.DirectoryPath);
            if (!File.Exists(executablePath))
            {
                Dispatcher.Invoke(() => instance.Status = "未找到 MAA.exe");
                return;
            }

            ApplyAdbPort(instance.DirectoryPath, instance.AdbPort);
            using var process = Process.Start(new ProcessStartInfo
            {
                FileName = executablePath,
                WorkingDirectory = instance.DirectoryPath,
                UseShellExecute = false,
            });

            Dispatcher.Invoke(() => {
                instance.ProcessId = process?.Id;
                instance.IsRunning = process is not null && !process.HasExited;
                instance.Status = instance.IsRunning ? "已启动" : "启动失败";
            });
        }
        catch (Exception ex)
        {
            Dispatcher.Invoke(() => instance.Status = "启动失败: " + ex.Message);
        }
    }

    private void StopInstance(ManagedInstance instance)
    {
        try
        {
            using var process = FindRunningProcess(instance);
            if (process is null)
            {
                Dispatcher.Invoke(() => {
                    instance.IsRunning = false;
                    instance.ProcessId = null;
                    instance.Status = "未运行";
                });
                return;
            }

            if (!process.CloseMainWindow())
            {
                process.Kill(entireProcessTree: true);
            }
            else if (!process.WaitForExit(5000))
            {
                process.Kill(entireProcessTree: true);
            }

            Dispatcher.Invoke(() => {
                instance.IsRunning = false;
                instance.ProcessId = null;
                instance.Status = "已关闭";
            });
        }
        catch (Exception ex)
        {
            Dispatcher.Invoke(() => instance.Status = "关闭失败: " + ex.Message);
        }
    }

    private async Task RunBusyAsync(string message, Func<Task> action)
    {
        if (_isBusy)
        {
            return;
        }

        _isBusy = true;
        IsEnabled = false;
        StatusMessage = message;
        try
        {
            await action();
        }
        catch (Exception ex)
        {
            StatusMessage = "操作失败: " + ex.Message;
        }
        finally
        {
            IsEnabled = true;
            _isBusy = false;
            NotifySummaryProperties();
        }
    }

    private bool ValidateReleasePackage()
    {
        if (!File.Exists(ReleasePackagePath) || !ReleasePackagePath.EndsWith(".zip", StringComparison.OrdinalIgnoreCase))
        {
            StatusMessage = "请选择 MAA release zip";
            return false;
        }

        return true;
    }

    private bool ValidateWorkspaceRoot()
    {
        if (string.IsNullOrWhiteSpace(WorkspaceRoot))
        {
            StatusMessage = "请选择实例总目录";
            return false;
        }

        try
        {
            Directory.CreateDirectory(WorkspaceRoot);
            return true;
        }
        catch (Exception ex)
        {
            StatusMessage = "实例总目录不可用: " + ex.Message;
            return false;
        }
    }

    private List<InstanceCreationPlan> BuildCreationPlan(int count)
    {
        Directory.CreateDirectory(WorkspaceRoot);
        var usedNames = new HashSet<string>(Instances.Select(static instance => instance.Name), StringComparer.OrdinalIgnoreCase);
        var usedPorts = new HashSet<int>(Instances.Select(static instance => instance.AdbPort).Where(static port => port > 0));
        foreach (var directory in Directory.EnumerateDirectories(WorkspaceRoot))
        {
            usedNames.Add(Path.GetFileName(directory));
            if (TryReadAdbPort(directory, out var port) && port > 0)
            {
                usedPorts.Add(port);
            }
        }

        var plan = new List<InstanceCreationPlan>(count);
        for (var index = 0; index < count; index++)
        {
            var name = GetNextInstanceName(usedNames);
            var port = GetNextPort(usedPorts);
            var directory = Path.Combine(WorkspaceRoot, name);
            if (Directory.Exists(directory))
            {
                throw new IOException($"实例目录已存在: {directory}");
            }

            usedNames.Add(name);
            usedPorts.Add(port);
            plan.Add(new InstanceCreationPlan(name, directory, port));
        }

        return plan;
    }

    private string GetNextInstanceName(HashSet<string> usedNames)
    {
        var prefix = SanitizeFileNamePrefix(InstanceNamePrefix);
        for (var index = 1; index < 10000; index++)
        {
            var name = $"{prefix}{index:D3}";
            if (!usedNames.Contains(name))
            {
                return name;
            }
        }

        throw new InvalidOperationException("无法生成新的实例名称");
    }

    private int GetNextPort(HashSet<int> usedPorts)
    {
        for (var port = StartAdbPort; port <= 65535; port += PortStep)
        {
            if (!usedPorts.Contains(port))
            {
                return port;
            }
        }

        throw new InvalidOperationException("没有可用的 ADB 端口");
    }

    private void RefreshInstances()
    {
        DiscoverInstancesFromWorkspace();
        RefreshConfiguredPorts();
        RefreshRunningStates();
        SaveState();
        NotifySummaryProperties();
    }

    private void DiscoverInstancesFromWorkspace()
    {
        if (!Directory.Exists(WorkspaceRoot))
        {
            return;
        }

        var knownDirectories = new HashSet<string>(Instances.Select(static instance => NormalizeDirectory(instance.DirectoryPath)), StringComparer.OrdinalIgnoreCase);
        foreach (var directory in Directory.EnumerateDirectories(WorkspaceRoot))
        {
            if (!File.Exists(GetExecutablePath(directory)))
            {
                continue;
            }

            var normalized = NormalizeDirectory(directory);
            if (knownDirectories.Contains(normalized))
            {
                continue;
            }

            TryReadAdbPort(directory, out var port);
            Instances.Add(CreateInstance(Path.GetFileName(directory), directory, port, "已发现"));
            knownDirectories.Add(normalized);
        }
    }

    private void RefreshConfiguredPorts()
    {
        foreach (var instance in Instances)
        {
            if (!Directory.Exists(instance.DirectoryPath))
            {
                instance.Status = "目录不存在";
                continue;
            }

            if (TryReadAdbPort(instance.DirectoryPath, out var port) && port > 0)
            {
                instance.AdbPort = port;
            }
        }
    }

    private void RefreshRunningStates()
    {
        foreach (var instance in Instances)
        {
            using var process = FindRunningProcess(instance);
            instance.IsRunning = process is not null;
            instance.ProcessId = process?.Id;
        }

        NotifySummaryProperties();
    }

    private IReadOnlyCollection<ManagedInstance> GetSelectedOrFocusedInstances()
    {
        var selected = Instances.Where(static instance => instance.IsSelected).ToList();
        if (selected.Count > 0)
        {
            return selected;
        }

        return SelectedInstance is null ? [] : [SelectedInstance];
    }

    private IReadOnlyCollection<ManagedInstance> GetSelectedOrAllInstances()
    {
        var selected = Instances.Where(static instance => instance.IsSelected).ToList();
        return selected.Count > 0 ? selected : Instances.ToList();
    }

    private void LoadState()
    {
        try
        {
            if (!File.Exists(_stateFile))
            {
                return;
            }

            var state = JsonSerializer.Deserialize<InstanceManagerState>(File.ReadAllText(_stateFile));
            if (state is null)
            {
                return;
            }

            _releasePackagePath = state.ReleasePackagePath ?? string.Empty;
            _workspaceRoot = string.IsNullOrWhiteSpace(state.WorkspaceRoot) ? _workspaceRoot : state.WorkspaceRoot;
            _instanceNamePrefix = string.IsNullOrWhiteSpace(state.InstanceNamePrefix) ? _instanceNamePrefix : state.InstanceNamePrefix;
            _newInstanceCount = Math.Clamp(state.NewInstanceCount, 1, 999);
            _cloneCount = Math.Clamp(state.CloneCount, 1, 999);
            _startAdbPort = Math.Clamp(state.StartAdbPort, 1, 65535);
            _portStep = Math.Clamp(state.PortStep, 1, 65535);

            Instances.Clear();
            foreach (var instance in state.Instances ?? [])
            {
                if (string.IsNullOrWhiteSpace(instance.DirectoryPath))
                {
                    continue;
                }

                Instances.Add(CreateInstance(instance.Name ?? Path.GetFileName(instance.DirectoryPath), instance.DirectoryPath, instance.AdbPort, instance.Status ?? string.Empty));
            }
        }
        catch (Exception ex)
        {
            StatusMessage = "读取实例管理配置失败: " + ex.Message;
        }
    }

    private void SaveState()
    {
        try
        {
            Directory.CreateDirectory(_configDirectory);
            var state = new InstanceManagerState
            {
                ReleasePackagePath = ReleasePackagePath,
                WorkspaceRoot = WorkspaceRoot,
                InstanceNamePrefix = InstanceNamePrefix,
                NewInstanceCount = NewInstanceCount,
                CloneCount = CloneCount,
                StartAdbPort = StartAdbPort,
                PortStep = PortStep,
                Instances = [.. Instances.Select(static instance => new ManagedInstanceState
                {
                    Name = instance.Name,
                    DirectoryPath = instance.DirectoryPath,
                    AdbPort = instance.AdbPort,
                    Status = instance.Status,
                })],
            };

            File.WriteAllText(_stateFile, JsonSerializer.Serialize(state, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch
        {
            // Saving state should not block instance operations.
        }
    }

    private void InstancesCollectionChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        if (e.NewItems is not null)
        {
            foreach (ManagedInstance instance in e.NewItems)
            {
                instance.PropertyChanged += InstancePropertyChanged;
            }
        }

        if (e.OldItems is not null)
        {
            foreach (ManagedInstance instance in e.OldItems)
            {
                instance.PropertyChanged -= InstancePropertyChanged;
            }
        }

        NotifySummaryProperties();
    }

    private void InstancePropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(ManagedInstance.IsSelected) or nameof(ManagedInstance.IsRunning))
        {
            NotifySummaryProperties();
        }
    }

    private void NotifySummaryProperties()
    {
        OnPropertyChanged(nameof(TotalCount));
        OnPropertyChanged(nameof(SelectedCount));
        OnPropertyChanged(nameof(RunningCount));
    }

    private static ManagedInstance CreateInstance(string name, string directoryPath, int adbPort, string status)
    {
        return new ManagedInstance
        {
            Name = name,
            DirectoryPath = directoryPath,
            AdbPort = adbPort,
            Status = status,
        };
    }

    private static void NormalizeExtractedReleaseRoot(string instanceDirectory)
    {
        if (File.Exists(GetExecutablePath(instanceDirectory)))
        {
            return;
        }

        var files = Directory.GetFiles(instanceDirectory);
        var directories = Directory.GetDirectories(instanceDirectory);
        if (files.Length != 0 || directories.Length != 1)
        {
            throw new FileNotFoundException("压缩包根目录下未找到 MAA.exe");
        }

        var nestedRoot = directories[0];
        if (!File.Exists(GetExecutablePath(nestedRoot)))
        {
            throw new FileNotFoundException("压缩包根目录下未找到 MAA.exe");
        }

        foreach (var entry in Directory.EnumerateFileSystemEntries(nestedRoot).ToList())
        {
            var target = Path.Combine(instanceDirectory, Path.GetFileName(entry));
            if (Directory.Exists(entry))
            {
                Directory.Move(entry, target);
            }
            else
            {
                File.Move(entry, target);
            }
        }

        Directory.Delete(nestedRoot, recursive: true);
    }

    private static void EnsureExecutableExists(string instanceDirectory)
    {
        if (!File.Exists(GetExecutablePath(instanceDirectory)))
        {
            throw new FileNotFoundException("实例目录下未找到 MAA.exe", GetExecutablePath(instanceDirectory));
        }
    }

    private static void ApplyAdbPort(string instanceDirectory, int adbPort)
    {
        if (adbPort <= 0)
        {
            return;
        }

        var configDirectory = Path.Combine(instanceDirectory, "config");
        Directory.CreateDirectory(configDirectory);
        var configFile = Path.Combine(configDirectory, "gui.json");
        var address = $"127.0.0.1:{adbPort}";

        JsonObject root;
        if (File.Exists(configFile))
        {
            root = JsonNode.Parse(File.ReadAllText(configFile)) as JsonObject ?? [];
        }
        else
        {
            root = [];
        }

        var current = root["Current"]?.GetValue<string>();
        if (string.IsNullOrWhiteSpace(current))
        {
            current = DefaultConfigurationName;
        }

        if (root["Configurations"] is not JsonObject configurations)
        {
            configurations = [];
            var migratedConfig = new JsonObject();
            foreach (var property in root.Where(static property => property.Key is not ("Current" or "Global" or "Configurations")).ToList())
            {
                migratedConfig[property.Key] = property.Value?.DeepClone();
                root.Remove(property.Key);
            }

            configurations[current] = migratedConfig;
            root["Configurations"] = configurations;
        }

        if (configurations[current] is not JsonObject currentConfig)
        {
            currentConfig = [];
            configurations[current] = currentConfig;
        }

        currentConfig[ConnectAddressKey] = address;
        currentConfig[ConnectAddressHistoryKey] = JsonSerializer.Serialize(new[] { address });
        root["Current"] = current;
        if (root["Global"] is not JsonObject)
        {
            root["Global"] = new JsonObject();
        }

        File.WriteAllText(configFile, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
    }

    private static bool TryReadAdbPort(string instanceDirectory, out int port)
    {
        port = 0;
        var configFile = Path.Combine(instanceDirectory, "config", "gui.json");
        if (!File.Exists(configFile))
        {
            return false;
        }

        try
        {
            var root = JsonNode.Parse(File.ReadAllText(configFile)) as JsonObject;
            var current = root?["Current"]?.GetValue<string>() ?? DefaultConfigurationName;
            var address = root?["Configurations"]?[current]?[ConnectAddressKey]?.GetValue<string>()
                          ?? root?[ConnectAddressKey]?.GetValue<string>();
            return TryParsePort(address, out port);
        }
        catch
        {
            return false;
        }
    }

    private static bool TryParsePort(string? address, out int port)
    {
        port = 0;
        if (string.IsNullOrWhiteSpace(address))
        {
            return false;
        }

        var colonIndex = address.LastIndexOf(':');
        var portText = colonIndex >= 0 ? address[(colonIndex + 1)..] : address;
        return int.TryParse(portText, out port) && port > 0;
    }

    private static void CopyDirectory(string sourceDirectory, string targetDirectory)
    {
        Directory.CreateDirectory(targetDirectory);
        foreach (var sourceSubdirectory in Directory.EnumerateDirectories(sourceDirectory, "*", SearchOption.AllDirectories))
        {
            Directory.CreateDirectory(Path.Combine(targetDirectory, Path.GetRelativePath(sourceDirectory, sourceSubdirectory)));
        }

        foreach (var sourceFile in Directory.EnumerateFiles(sourceDirectory, "*", SearchOption.AllDirectories))
        {
            var relativePath = Path.GetRelativePath(sourceDirectory, sourceFile);
            var targetFile = Path.Combine(targetDirectory, relativePath);
            var targetParent = Path.GetDirectoryName(targetFile);
            if (!string.IsNullOrEmpty(targetParent))
            {
                Directory.CreateDirectory(targetParent);
            }

            File.Copy(sourceFile, targetFile, overwrite: false);
        }
    }

    private static void TryDeleteDirectory(string directoryPath)
    {
        try
        {
            if (Directory.Exists(directoryPath))
            {
                Directory.Delete(directoryPath, recursive: true);
            }
        }
        catch
        {
            // Best effort cleanup after failed extraction/copy.
        }
    }

    private static Process? FindRunningProcess(ManagedInstance instance)
    {
        var executablePath = GetExecutablePath(instance.DirectoryPath);
        if (instance.ProcessId is int processId)
        {
            try
            {
                var process = Process.GetProcessById(processId);
                if (!process.HasExited && IsSamePath(GetMainModuleFileName(process), executablePath))
                {
                    return process;
                }

                process.Dispose();
            }
            catch
            {
                // ignored
            }
        }

        foreach (var process in Process.GetProcessesByName(Path.GetFileNameWithoutExtension(ExecutableName)))
        {
            try
            {
                if (!process.HasExited && IsSamePath(GetMainModuleFileName(process), executablePath))
                {
                    return process;
                }
            }
            catch
            {
                // ignored
            }

            process.Dispose();
        }

        return null;
    }

    private static string? GetMainModuleFileName(Process process)
    {
        try
        {
            return process.MainModule?.FileName;
        }
        catch
        {
            return null;
        }
    }

    private static string GetExecutablePath(string directoryPath)
    {
        return Path.Combine(directoryPath, ExecutableName);
    }

    private static bool IsSamePath(string? left, string? right)
    {
        if (string.IsNullOrWhiteSpace(left) || string.IsNullOrWhiteSpace(right))
        {
            return false;
        }

        return string.Equals(Path.GetFullPath(left), Path.GetFullPath(right), StringComparison.OrdinalIgnoreCase);
    }

    private static string NormalizeDirectory(string directoryPath)
    {
        return Path.GetFullPath(directoryPath)
            .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
    }

    private static string SanitizeFileNamePrefix(string prefix)
    {
        var invalidChars = Path.GetInvalidFileNameChars();
        var sanitized = new string((prefix ?? string.Empty).Select(ch => invalidChars.Contains(ch) ? '_' : ch).ToArray());
        return string.IsNullOrWhiteSpace(sanitized) ? "MAA-" : sanitized;
    }

    private static void OpenFolder(string directoryPath)
    {
        Process.Start(new ProcessStartInfo
        {
            FileName = directoryPath,
            UseShellExecute = true,
        });
    }

    private bool SetField<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            return false;
        }

        field = value;
        OnPropertyChanged(propertyName);
        return true;
    }

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }

    private sealed record InstanceCreationPlan(string Name, string DirectoryPath, int AdbPort);
}

public sealed class ManagedInstance : INotifyPropertyChanged
{
    private bool _isSelected;
    private string _name = string.Empty;
    private string _directoryPath = string.Empty;
    private int _adbPort;
    private bool _isRunning;
    private int? _processId;
    private string _status = string.Empty;

    public event PropertyChangedEventHandler? PropertyChanged;

    public bool IsSelected
    {
        get => _isSelected;
        set => SetField(ref _isSelected, value);
    }

    public string Name
    {
        get => _name;
        set => SetField(ref _name, value);
    }

    public string DirectoryPath
    {
        get => _directoryPath;
        set => SetField(ref _directoryPath, value);
    }

    public int AdbPort
    {
        get => _adbPort;
        set {
            if (SetField(ref _adbPort, value))
            {
                OnPropertyChanged(nameof(ConnectAddress));
            }
        }
    }

    public string ConnectAddress => AdbPort > 0 ? $"127.0.0.1:{AdbPort}" : string.Empty;

    public bool IsRunning
    {
        get => _isRunning;
        set {
            if (SetField(ref _isRunning, value))
            {
                OnPropertyChanged(nameof(StateText));
            }
        }
    }

    public string StateText => IsRunning ? "运行中" : "已停止";

    public int? ProcessId
    {
        get => _processId;
        set => SetField(ref _processId, value);
    }

    public string Status
    {
        get => _status;
        set => SetField(ref _status, value);
    }

    private bool SetField<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            return false;
        }

        field = value;
        OnPropertyChanged(propertyName);
        return true;
    }

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}

public sealed class InstanceManagerState
{
    public string? ReleasePackagePath { get; set; }

    public string? WorkspaceRoot { get; set; }

    public string? InstanceNamePrefix { get; set; }

    public int NewInstanceCount { get; set; } = 1;

    public int CloneCount { get; set; } = 1;

    public int StartAdbPort { get; set; } = 16384;

    public int PortStep { get; set; } = 32;

    public List<ManagedInstanceState>? Instances { get; set; }
}

public sealed class ManagedInstanceState
{
    public string? Name { get; set; }

    public string? DirectoryPath { get; set; }

    public int AdbPort { get; set; }

    public string? Status { get; set; }
}
