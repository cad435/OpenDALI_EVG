using Avalonia.Controls;
using Avalonia.Platform.Storage;
using EVG_Updater.ViewModels;

namespace EVG_Updater.Views;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        // The file dialog needs a TopLevel, which the view has and the VM should not.
        DataContextChanged += (_, _) =>
        {
            if (DataContext is MainViewModel vm) vm.PickFirmwareFile = PickFirmwareAsync;
        };
    }

    private async Task<string?> PickFirmwareAsync()
    {
        var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Select firmware binary",
            AllowMultiple = false,
            FileTypeFilter =
            [
                new FilePickerFileType("Binary files") { Patterns = ["*.bin"] },
                FilePickerFileTypes.All,
            ],
        });
        return files.Count > 0 ? files[0].TryGetLocalPath() : null;
    }
}
