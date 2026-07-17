using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace CKFlip3D.Settings.Views;

public partial class AboutPage : UserControl
{
    public AboutPage()
    {
        InitializeComponent();
    }

    private void License_Click(object sender, RoutedEventArgs e)
    {
        if (Window.GetWindow(this) is not MainWindow main) return;

        // PolyForm Noncommercial 1.0.0 — summary + Required Notice + the
        // license URL.  Per the license's Notices section, passing on the
        // URL together with the Required Notice line satisfies the notice
        // requirement; the full text also ships as LICENSE.md in the repo.
        var body = new TextBlock
        {
            TextWrapping = TextWrapping.Wrap,
            FontFamily = new FontFamily("Segoe UI"),
            FontSize = 12.5,
            LineHeight = 19,
            Foreground = (Brush)FindResource("TextPrimaryBrush"),
            Text =
                "CKFlip3D — License\n\n" +
                "Licensed under the PolyForm Noncommercial License 1.0.0.\n" +
                "Full text:  https://polyformproject.org/licenses/noncommercial/1.0.0\n\n" +
                "Required Notice: Copyright © 2026 Karol Cymerman (CYMERKAROL) — " +
                "https://github.com/CYMERKAROL/CKFlip3D\n\n" +
                "In short:\n" +
                "  •  You may use, copy, modify and share this software freely for any " +
                "noncommercial purpose — personal use, study, research, hobby projects, " +
                "and use by noncommercial organizations.\n" +
                "  •  Anyone who receives a copy from you must also receive these license " +
                "terms (or the URL above) together with the Required Notice line.\n" +
                "  •  Commercial use is not permitted under this license. For commercial " +
                "licensing, contact the author via GitHub.\n" +
                "  •  The software is provided \"as is\", without any warranty; the " +
                "licensor is not liable for any damages arising from its use.\n\n" +
                "The summary above is for convenience only — the PolyForm Noncommercial " +
                "License 1.0.0 text is the sole authoritative source of the terms.\n\n" +
                "CKFlip3D is an independent project. It is not affiliated with, endorsed by, " +
                "or sponsored by Microsoft Corporation. All visual elements of this application " +
                "are original, procedurally drawn work inspired by — but not copied from — the " +
                "Windows Aero design language.\n\n" +
                "Third-party components:  none — the application links only against " +
                "operating-system libraries and the .NET runtime.\n",
        };

        main.ShowModal("License", body, ("Close", true, null));
    }

    private void GitHub_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            Process.Start(new ProcessStartInfo("https://github.com/CYMERKAROL") { UseShellExecute = true });
        }
        catch
        {
            // no default browser registered; nothing sensible to do
        }
    }
}
