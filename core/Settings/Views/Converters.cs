using System.Globalization;
using System.Windows.Data;

namespace CKFlip3D.Settings.Views;

/// <summary>Multiplies a numeric binding value by the converter parameter (e.g. 0.01 for percent → 0..1).</summary>
public sealed class ScaleConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        double v = System.Convert.ToDouble(value, CultureInfo.InvariantCulture);
        double factor = System.Convert.ToDouble(parameter, CultureInfo.InvariantCulture);
        return v * factor;
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
    {
        double v = System.Convert.ToDouble(value, CultureInfo.InvariantCulture);
        double factor = System.Convert.ToDouble(parameter, CultureInfo.InvariantCulture);
        return factor == 0 ? 0 : v / factor;
    }
}
