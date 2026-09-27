using System.Globalization;
using Avalonia.Data.Converters;
using Avalonia.Media;
using EvgFlasher.ViewModels;

namespace EvgFlasher.Converters;

/// <summary>Maps the run outcome to the accent colour of the status banner.</summary>
public sealed class StatusBrushConverter : IValueConverter
{
    public static readonly StatusBrushConverter Instance = new();

    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        (value as StatusKind?) switch
        {
            StatusKind.Pass  => new SolidColorBrush(Color.Parse("#2E7D32")),
            StatusKind.Fail  => new SolidColorBrush(Color.Parse("#C62828")),
            StatusKind.Error => new SolidColorBrush(Color.Parse("#EF6C00")),
            StatusKind.Busy  => new SolidColorBrush(Color.Parse("#1565C0")),
            _                => new SolidColorBrush(Color.Parse("#455A64")),
        };

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        throw new NotSupportedException();
}

/// <summary>True when the given step number is the one currently running.</summary>
public sealed class StepActiveConverter : IValueConverter
{
    public static readonly StepActiveConverter Instance = new();

    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        value is int current && parameter is string s && int.TryParse(s, out var step) && current == step;

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        throw new NotSupportedException();
}

/// <summary>Green pill while a chip is attached to the WCH-Link, grey otherwise.</summary>
public sealed class ChipBrushConverter : IValueConverter
{
    public static readonly ChipBrushConverter Instance = new();

    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        new SolidColorBrush(Color.Parse(value is true ? "#2E7D32" : "#546E7A"));

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        throw new NotSupportedException();
}

/// <summary>Caption of the chip pill.</summary>
public sealed class ChipTextConverter : IValueConverter
{
    public static readonly ChipTextConverter Instance = new();

    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        value is true ? "CHIP CONNECTED" : "NO CHIP";

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        throw new NotSupportedException();
}
