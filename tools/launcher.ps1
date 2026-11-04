# The graphical launcher for the native build (decomp/build/dra2_native.exe), started by "DRA 2.exe".
# A WPF window: Home (status, quick settings), Display, Audio, Controls, Saves and Advanced pages, and PLAY.
# Settings are kept in <repo>\launcher_settings.json; saves in <repo>\saves; logs in <repo>\logs.
# Needs Windows PowerShell 5.1 in STA mode ("DRA 2.exe" starts it that way).
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Windows.Forms
# An app ID of its own, so the taskbar shows the launcher's icon rather than grouping it under PowerShell's
try {
    Add-Type -Namespace DRA2 -Name Shell -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("shell32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int SetCurrentProcessExplicitAppUserModelID(string id);
'@
    $null = [DRA2.Shell]::SetCurrentProcessExplicitAppUserModelID('BlueOptimist.DRA2.Launcher')
} catch { }
trap { [Windows.MessageBox]::Show("The launcher stopped with an error:

$_", 'DRA2 launcher', 'OK', 'Error') | Out-Null; exit 1 }

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'decomp\build\dra2_native.exe'
$logs = Join-Path $root 'logs'
$settingsFile = Join-Path $root 'launcher_settings.json'
$backups = Join-Path $root 'save_backups'

# ------------------------------------------------------------------------------------------------- settings --
$settings = [ordered]@{
    display = '2'                                 # 1..4 (window scale) or fullscreen
    aspect = '4:3'                                # the picture's shape (DRA2_ASPECT): 4:3, 16:10, 16:9 or 21:9
    resolution = '1080'                           # lines drawn (DRA2_RESOLUTION): 480 (original), 720, 1080, 1440, 2160
    msaa = '4'                                    # anti-aliasing samples (DRA2_MSAA): 0 (original), 2, 4, 8
    vsync = $true                                 # wait for the display's refresh to show each frame (DRA2_VSYNC)
    keys = ''                                     # keyboard bindings (DRA2_KEYS): control=vk[+vk],... in hex; '' = defaults
    pad = ''                                      # controller bindings (DRA2_PAD): control=button,...; '' = defaults
    soft = $true                                  # the Xbox's soft display (DRA2_SOFT)
    sound = $true
    quickIntro = $true                            # the copyright screen for a second and no studio logos (DRA2_QUICKINTRO)
    record = $false
    hideWhilePlaying = $true
    dataPath = (Join-Path $root 'extracted')
    savesPath = (Join-Path $root 'saves')
}
if (Test-Path $settingsFile) {
    try {
        $saved = Get-Content $settingsFile -Raw | ConvertFrom-Json
        foreach ($p in $saved.PSObject.Properties) { if ($settings.Contains($p.Name)) { $settings[$p.Name] = $p.Value } }
    } catch { }
}
function Save-Settings { $settings | ConvertTo-Json | Set-Content $settingsFile -Encoding UTF8 }

# ------------------------------------------------------------------------------------------------------ UI --
[xml]$xaml = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="Digimon Rumble Arena 2 - Launcher" Width="1120" Height="700" WindowStyle="None" ResizeMode="CanMinimize"
        WindowStartupLocation="CenterScreen" Background="#050A1C" FontFamily="Segoe UI" Foreground="#DDE8FF"
        SnapsToDevicePixels="True" UseLayoutRounding="True">
  <Window.Resources>
    <SolidColorBrush x:Key="Accent" Color="#FFB21E"/>
    <SolidColorBrush x:Key="Cyan" Color="#38C8FF"/>
    <SolidColorBrush x:Key="Green" Color="#2BE07A"/>
    <SolidColorBrush x:Key="Panel" Color="#C00A1430"/>
    <SolidColorBrush x:Key="Line" Color="#3326A8FF"/>
    <LinearGradientBrush x:Key="Fire" StartPoint="0,0" EndPoint="0,1">
      <GradientStop Color="#FFE45C" Offset="0"/><GradientStop Color="#FF9A1F" Offset="0.5"/><GradientStop Color="#E8331B" Offset="1"/>
    </LinearGradientBrush>

    <Style x:Key="Heading" TargetType="TextBlock">
      <Setter Property="FontFamily" Value="Segoe UI Black"/><Setter Property="FontStyle" Value="Italic"/>
      <Setter Property="FontSize" Value="30"/><Setter Property="Foreground" Value="{StaticResource Fire}"/>
      <Setter Property="Margin" Value="0,0,0,4"/>
      <Setter Property="Effect"><Setter.Value><DropShadowEffect Color="#000000" ShadowDepth="3" BlurRadius="6" Opacity="0.9"/></Setter.Value></Setter>
    </Style>
    <Style x:Key="Sub" TargetType="TextBlock">
      <Setter Property="Foreground" Value="#8FA6D6"/><Setter Property="FontSize" Value="13"/><Setter Property="TextWrapping" Value="Wrap"/>
    </Style>
    <Style x:Key="Label" TargetType="TextBlock">
      <Setter Property="FontFamily" Value="Segoe UI Semibold"/><Setter Property="FontSize" Value="13"/>
      <Setter Property="Foreground" Value="{StaticResource Cyan}"/><Setter Property="Margin" Value="0,18,0,8"/>
    </Style>

    <!-- left navigation: a radio button with an accent bar -->
    <Style x:Key="Nav" TargetType="RadioButton">
      <Setter Property="Foreground" Value="#7F93C2"/><Setter Property="FontFamily" Value="Segoe UI Black"/>
      <Setter Property="FontStyle" Value="Italic"/><Setter Property="FontSize" Value="17"/><Setter Property="Cursor" Value="Hand"/>
      <Setter Property="GroupName" Value="nav"/>
      <Setter Property="Template"><Setter.Value>
        <ControlTemplate TargetType="RadioButton">
          <Grid Background="Transparent" Height="48">
            <Border x:Name="glow" Opacity="0" CornerRadius="0,24,24,0" Margin="0,4,18,4">
              <Border.Background><LinearGradientBrush StartPoint="0,0" EndPoint="1,0">
                <GradientStop Color="#55FF9A1F" Offset="0"/><GradientStop Color="#00FF9A1F" Offset="1"/></LinearGradientBrush></Border.Background>
            </Border>
            <Rectangle x:Name="bar" Width="5" HorizontalAlignment="Left" Fill="{StaticResource Fire}" Opacity="0" Margin="0,8"/>
            <ContentPresenter Margin="28,0,0,0" VerticalAlignment="Center"/>
          </Grid>
          <ControlTemplate.Triggers>
            <Trigger Property="IsChecked" Value="True">
              <Setter TargetName="bar" Property="Opacity" Value="1"/><Setter TargetName="glow" Property="Opacity" Value="1"/>
              <Setter Property="Foreground" Value="#FFFFFF"/>
            </Trigger>
            <Trigger Property="IsMouseOver" Value="True"><Setter Property="Foreground" Value="#FFFFFF"/></Trigger>
          </ControlTemplate.Triggers>
        </ControlTemplate>
      </Setter.Value></Setter>
    </Style>

    <!-- chips: mutually exclusive choices -->
    <Style x:Key="Chip" TargetType="RadioButton">
      <Setter Property="Foreground" Value="#BCCBF0"/><Setter Property="FontFamily" Value="Segoe UI Semibold"/>
      <Setter Property="FontSize" Value="13"/><Setter Property="Cursor" Value="Hand"/><Setter Property="Margin" Value="0,0,10,10"/>
      <Setter Property="Template"><Setter.Value>
        <ControlTemplate TargetType="RadioButton">
          <Border x:Name="b" CornerRadius="6" BorderThickness="1" BorderBrush="#3A5A9A" Background="#AA0C1838" Padding="16,9">
            <ContentPresenter HorizontalAlignment="Center"/>
          </Border>
          <ControlTemplate.Triggers>
            <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="b" Property="BorderBrush" Value="#38C8FF"/></Trigger>
            <Trigger Property="IsChecked" Value="True">
              <Setter TargetName="b" Property="Background" Value="{StaticResource Fire}"/>
              <Setter TargetName="b" Property="BorderBrush" Value="#FFE45C"/><Setter Property="Foreground" Value="#1A0A00"/>
            </Trigger>
          </ControlTemplate.Triggers>
        </ControlTemplate>
      </Setter.Value></Setter>
    </Style>

    <!-- on/off switch -->
    <Style x:Key="Switch" TargetType="CheckBox">
      <Setter Property="Foreground" Value="#DDE8FF"/><Setter Property="FontSize" Value="14"/><Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Margin" Value="0,0,0,12"/>
      <Setter Property="Template"><Setter.Value>
        <ControlTemplate TargetType="CheckBox">
          <StackPanel Orientation="Horizontal" Background="Transparent">
            <Grid Width="46" Height="24">
              <Border x:Name="track" CornerRadius="12" Background="#22325A" BorderBrush="#3A5A9A" BorderThickness="1"/>
              <Ellipse x:Name="knob" Width="18" Height="18" Fill="#8FA6D6" HorizontalAlignment="Left" Margin="3,0,0,0"/>
            </Grid>
            <ContentPresenter Margin="12,0,0,0" VerticalAlignment="Center"/>
          </StackPanel>
          <ControlTemplate.Triggers>
            <Trigger Property="IsChecked" Value="True">
              <Setter TargetName="track" Property="Background" Value="{StaticResource Fire}"/>
              <Setter TargetName="track" Property="BorderBrush" Value="#FFE45C"/>
              <Setter TargetName="knob" Property="HorizontalAlignment" Value="Right"/>
              <Setter TargetName="knob" Property="Margin" Value="0,0,3,0"/><Setter TargetName="knob" Property="Fill" Value="#FFFFFF"/>
            </Trigger>
          </ControlTemplate.Triggers>
        </ControlTemplate>
      </Setter.Value></Setter>
    </Style>

    <!-- plain buttons -->
    <Style x:Key="Btn" TargetType="Button">
      <Setter Property="Foreground" Value="#DDE8FF"/><Setter Property="FontFamily" Value="Segoe UI Semibold"/>
      <Setter Property="FontSize" Value="13"/><Setter Property="Cursor" Value="Hand"/><Setter Property="Margin" Value="0,0,10,10"/>
      <Setter Property="Template"><Setter.Value>
        <ControlTemplate TargetType="Button">
          <Border x:Name="b" CornerRadius="6" BorderThickness="1" BorderBrush="#3A5A9A" Background="#CC102048" Padding="16,9">
            <ContentPresenter HorizontalAlignment="Center"/>
          </Border>
          <ControlTemplate.Triggers>
            <Trigger Property="IsMouseOver" Value="True">
              <Setter TargetName="b" Property="BorderBrush" Value="#38C8FF"/><Setter TargetName="b" Property="Background" Value="#CC17306A"/>
            </Trigger>
            <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.45"/></Trigger>
          </ControlTemplate.Triggers>
        </ControlTemplate>
      </Setter.Value></Setter>
    </Style>

    <!-- a binding on the Controls page: compact, highlighted while it waits for a key -->
    <Style x:Key="Slot" TargetType="Button">
      <Setter Property="Foreground" Value="#DDE8FF"/><Setter Property="FontFamily" Value="Segoe UI Semibold"/>
      <Setter Property="FontSize" Value="12"/><Setter Property="Cursor" Value="Hand"/><Setter Property="Margin" Value="0,1,6,1"/>
      <Setter Property="Template"><Setter.Value>
        <ControlTemplate TargetType="Button">
          <Border x:Name="b" CornerRadius="5" BorderThickness="1" BorderBrush="#3A5A9A" Background="#CC102048" Padding="8,2">
            <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
          </Border>
          <ControlTemplate.Triggers>
            <Trigger Property="IsMouseOver" Value="True">
              <Setter TargetName="b" Property="BorderBrush" Value="#38C8FF"/><Setter TargetName="b" Property="Background" Value="#CC17306A"/>
            </Trigger>
            <Trigger Property="Tag" Value="waiting">
              <Setter TargetName="b" Property="BorderBrush" Value="#FFB21E"/><Setter TargetName="b" Property="Background" Value="#CC4A3208"/>
            </Trigger>
          </ControlTemplate.Triggers>
        </ControlTemplate>
      </Setter.Value></Setter>
    </Style>

    <!-- title bar buttons -->
    <Style x:Key="Caption" TargetType="Button">
      <Setter Property="Foreground" Value="#8FA6D6"/><Setter Property="FontSize" Value="14"/><Setter Property="Width" Value="46"/>
      <Setter Property="Height" Value="34"/><Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template"><Setter.Value>
        <ControlTemplate TargetType="Button">
          <Border x:Name="b" Background="Transparent"><ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/></Border>
          <ControlTemplate.Triggers>
            <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="b" Property="Background" Value="#33FFFFFF"/>
              <Setter Property="Foreground" Value="#FFFFFF"/></Trigger>
          </ControlTemplate.Triggers>
        </ControlTemplate>
      </Setter.Value></Setter>
    </Style>

    <!-- the PLAY button: a hexagon -->
    <Style x:Key="Play" TargetType="Button">
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template"><Setter.Value>
        <ControlTemplate TargetType="Button">
          <Grid Width="250" Height="74">
            <Polygon x:Name="hex" Points="22,0 228,0 250,37 228,74 22,74 0,37" Fill="{StaticResource Fire}" Stroke="#FFF2A0" StrokeThickness="2">
              <Polygon.Effect><DropShadowEffect x:Name="glow" Color="#FF8A00" BlurRadius="22" ShadowDepth="0" Opacity="0.7"/></Polygon.Effect>
            </Polygon>
            <Polygon Points="28,5 222,5 238,30 12,30" Fill="#38FFFFFF"/>
            <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
          </Grid>
          <ControlTemplate.Triggers>
            <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="hex" Property="Stroke" Value="#FFFFFF"/>
              <Setter TargetName="hex" Property="StrokeThickness" Value="3"/></Trigger>
            <Trigger Property="IsPressed" Value="True"><Setter Property="RenderTransform"><Setter.Value>
              <ScaleTransform ScaleX="0.97" ScaleY="0.97" CenterX="125" CenterY="37"/></Setter.Value></Setter></Trigger>
            <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.55"/></Trigger>
          </ControlTemplate.Triggers>
        </ControlTemplate>
      </Setter.Value></Setter>
    </Style>

    <Style TargetType="TextBox">
      <Setter Property="Background" Value="#CC0A1430"/><Setter Property="Foreground" Value="#DDE8FF"/>
      <Setter Property="BorderBrush" Value="#3A5A9A"/><Setter Property="Padding" Value="8,7"/><Setter Property="FontSize" Value="13"/>
      <Setter Property="CaretBrush" Value="#FFFFFF"/>
    </Style>

    <Style x:Key="Card" TargetType="Border">
      <Setter Property="Background" Value="{StaticResource Panel}"/><Setter Property="BorderBrush" Value="#2E4C88"/>
      <Setter Property="BorderThickness" Value="1"/><Setter Property="CornerRadius" Value="10"/><Setter Property="Padding" Value="18,14"/>
    </Style>
  </Window.Resources>

  <Grid>
    <!-- ======================================================================= the animated background === -->
    <Canvas x:Name="bg" IsHitTestVisible="False" ClipToBounds="True">
      <Canvas.Background>
        <RadialGradientBrush Center="0.66,0.47" GradientOrigin="0.66,0.47" RadiusX="0.75" RadiusY="0.9">
          <GradientStop Color="#0A1B4A" Offset="0"/><GradientStop Color="#071232" Offset="0.45"/><GradientStop Color="#03060F" Offset="1"/>
        </RadialGradientBrush>
      </Canvas.Background>
      <Rectangle Width="1120" Height="700" Opacity="0.13" CacheMode="BitmapCache">
        <Rectangle.Fill>
          <DrawingBrush TileMode="Tile" Viewport="0,0,28,50" ViewportUnits="Absolute" Stretch="None">
            <DrawingBrush.Drawing>
              <GeometryDrawing Geometry="M28 66L0 50L0 16L28 0L56 16L56 50L28 66L28 100">
                <GeometryDrawing.Pen><Pen Brush="#38C8FF" Thickness="2"/></GeometryDrawing.Pen>
              </GeometryDrawing>
            </DrawingBrush.Drawing>
          </DrawingBrush>
        </Rectangle.Fill>
      </Rectangle>
      <!-- the tunnel: rings of "binary" dashes turning both ways around (740, 330) -->
      <Ellipse x:Name="ring1" CacheMode="BitmapCache" Width="760" Height="760" Canvas.Left="360" Canvas.Top="-50" Stroke="#2BE07A" StrokeThickness="10" Opacity="0.16"
               StrokeDashArray="0.4 1.2 1.6 0.8 0.4 2.4 0.4 0.8" RenderTransformOrigin="0.5,0.5"><Ellipse.RenderTransform><RotateTransform/></Ellipse.RenderTransform></Ellipse>
      <Ellipse x:Name="ring2" CacheMode="BitmapCache" Width="560" Height="560" Canvas.Left="460" Canvas.Top="50" Stroke="#2BE07A" StrokeThickness="8" Opacity="0.22"
               StrokeDashArray="1.5 0.6 0.4 0.6 0.4 1.8 2 0.6" RenderTransformOrigin="0.5,0.5"><Ellipse.RenderTransform><RotateTransform/></Ellipse.RenderTransform></Ellipse>
      <Ellipse x:Name="ring3" CacheMode="BitmapCache" Width="390" Height="390" Canvas.Left="545" Canvas.Top="135" Stroke="#38C8FF" StrokeThickness="14" Opacity="0.18"
               StrokeDashArray="0.8 0.5" RenderTransformOrigin="0.5,0.5"><Ellipse.RenderTransform><RotateTransform/></Ellipse.RenderTransform></Ellipse>
      <Ellipse x:Name="ring4" CacheMode="BitmapCache" Width="250" Height="250" Canvas.Left="615" Canvas.Top="205" Stroke="#2BE07A" StrokeThickness="6" Opacity="0.3"
               StrokeDashArray="0.5 1 2 1 0.5 0.5" RenderTransformOrigin="0.5,0.5"><Ellipse.RenderTransform><RotateTransform/></Ellipse.RenderTransform></Ellipse>
      <Ellipse x:Name="ring5" CacheMode="BitmapCache" Width="140" Height="140" Canvas.Left="670" Canvas.Top="260" Stroke="#38C8FF" StrokeThickness="3" Opacity="0.35"
               StrokeDashArray="3 2" RenderTransformOrigin="0.5,0.5"><Ellipse.RenderTransform><RotateTransform/></Ellipse.RenderTransform></Ellipse>
      <Ellipse Width="90" Height="90" Canvas.Left="695" Canvas.Top="285">
        <Ellipse.Fill><RadialGradientBrush><GradientStop Color="#000000" Offset="0.3"/><GradientStop Color="#00000000" Offset="1"/></RadialGradientBrush></Ellipse.Fill>
      </Ellipse>
      <Canvas x:Name="sparks"/>
      <!-- darken behind the text on the left -->
      <Rectangle Width="1120" Height="700" CacheMode="BitmapCache">
        <Rectangle.Fill><LinearGradientBrush StartPoint="0,0" EndPoint="1,0">
          <GradientStop Color="#F0030713" Offset="0"/><GradientStop Color="#C0030713" Offset="0.2"/>
          <GradientStop Color="#40030713" Offset="0.62"/><GradientStop Color="#10030713" Offset="1"/></LinearGradientBrush></Rectangle.Fill>
      </Rectangle>
    </Canvas>

    <Grid>
      <Grid.RowDefinitions><RowDefinition Height="34"/><RowDefinition Height="*"/><RowDefinition Height="104"/></Grid.RowDefinitions>
      <Grid.ColumnDefinitions><ColumnDefinition Width="210"/><ColumnDefinition Width="*"/></Grid.ColumnDefinitions>

      <!-- title bar -->
      <Border x:Name="titleBar" Grid.ColumnSpan="2" Background="#01000000">
        <DockPanel>
          <Button x:Name="closeBtn" DockPanel.Dock="Right" Style="{StaticResource Caption}" Content="&#x2715;"/>
          <Button x:Name="minBtn" DockPanel.Dock="Right" Style="{StaticResource Caption}" Content="&#x2014;"/>
          <Image x:Name="titleIcon" DockPanel.Dock="Left" Width="18" Height="18" Margin="12,0,0,0"
                 VerticalAlignment="Center" RenderOptions.BitmapScalingMode="HighQuality"/>
          <TextBlock Text="DRA2  ·  WINDOWS PORT LAUNCHER" Margin="9,0" VerticalAlignment="Center" FontSize="11"
                     FontFamily="Segoe UI Semibold" Foreground="#5D74A8"/>
        </DockPanel>
      </Border>

      <!-- navigation -->
      <StackPanel Grid.Row="1" Margin="0,18,0,0">
        <RadioButton x:Name="navHome" Style="{StaticResource Nav}" Content="HOME" IsChecked="True"/>
        <RadioButton x:Name="navDisplay" Style="{StaticResource Nav}" Content="DISPLAY"/>
        <RadioButton x:Name="navAudio" Style="{StaticResource Nav}" Content="AUDIO"/>
        <RadioButton x:Name="navControls" Style="{StaticResource Nav}" Content="CONTROLS"/>
        <RadioButton x:Name="navSaves" Style="{StaticResource Nav}" Content="SAVES"/>
        <RadioButton x:Name="navAdvanced" Style="{StaticResource Nav}" Content="ADVANCED"/>
      </StackPanel>

      <!-- pages -->
      <Grid Grid.Row="1" Grid.Column="1" Margin="10,14,40,0">

        <!-- HOME -->
        <StackPanel x:Name="pageHome">
          <TextBlock Text="DIGIMON" FontFamily="Segoe UI Black" FontStyle="Italic" FontSize="20" Foreground="#38C8FF" Margin="4,0,0,-12"/>
          <TextBlock Text="RUMBLE ARENA 2" FontFamily="Segoe UI Black" FontStyle="Italic" FontSize="62" Foreground="{StaticResource Fire}">
            <TextBlock.Effect><DropShadowEffect Color="#000000" ShadowDepth="4" BlurRadius="10" Opacity="1"/></TextBlock.Effect>
          </TextBlock>
          <TextBlock Text="Windows port  ·  native build" Style="{StaticResource Sub}" FontSize="15" Margin="4,-4,0,22"/>
          <UniformGrid Columns="3" Width="640" HorizontalAlignment="Left">
            <Border Style="{StaticResource Card}" Margin="0,0,12,0">
              <StackPanel><TextBlock Text="GAME DATA" Style="{StaticResource Sub}" FontSize="11"/>
                <TextBlock x:Name="dataStatus" FontSize="17" FontFamily="Segoe UI Semibold" Margin="0,4,0,0"/>
                <TextBlock x:Name="dataDetail" Style="{StaticResource Sub}" FontSize="11"/></StackPanel>
            </Border>
            <Border Style="{StaticResource Card}" Margin="0,0,12,0">
              <StackPanel><TextBlock Text="GAME BUILD" Style="{StaticResource Sub}" FontSize="11"/>
                <TextBlock x:Name="buildStatus" FontSize="17" FontFamily="Segoe UI Semibold" Margin="0,4,0,0"/>
                <TextBlock x:Name="buildDetail" Style="{StaticResource Sub}" FontSize="11"/></StackPanel>
            </Border>
            <Border Style="{StaticResource Card}">
              <StackPanel><TextBlock Text="SAVES" Style="{StaticResource Sub}" FontSize="11"/>
                <TextBlock x:Name="saveStatus" FontSize="17" FontFamily="Segoe UI Semibold" Margin="0,4,0,0"/>
                <TextBlock x:Name="saveDetail" Style="{StaticResource Sub}" FontSize="11"/></StackPanel>
            </Border>
          </UniformGrid>
          <TextBlock Text="QUICK SETTINGS" Style="{StaticResource Label}" Margin="0,26,0,10"/>
          <WrapPanel x:Name="quickDisplay"/>
        </StackPanel>

        <!-- DISPLAY -->
        <StackPanel x:Name="pageDisplay" Visibility="Collapsed">
          <TextBlock Text="DISPLAY" Style="{StaticResource Heading}"/>
          <TextBlock Style="{StaticResource Sub}">In the game, <Run Foreground="#FFB21E" FontWeight="Bold">Alt+Enter</Run> or <Run Foreground="#FFB21E" FontWeight="Bold">F11</Run> switches between the window and fullscreen. The window can be resized; black bars fill the sides.</TextBlock>
          <TextBlock Text="DISPLAY MODE" Style="{StaticResource Label}"/>
          <WrapPanel x:Name="displayModes"/>
          <TextBlock Text="RESOLUTION  (sharper 3D scenes, HUD and text; the game's own textures stay as they are)" Style="{StaticResource Label}"/>
          <WrapPanel x:Name="resolutionModes"/>
          <TextBlock Text="ANTI-ALIASING  (MSAA: smooths the stair-stepped edges of the 3D scenes)" Style="{StaticResource Label}"/>
          <WrapPanel x:Name="msaaModes"/>
          <TextBlock Text="PICTURE SHAPE  (wider shows more of each scene at the sides; the HUD and menus keep their shape)" Style="{StaticResource Label}"/>
          <WrapPanel x:Name="aspectModes"/>
          <TextBlock Text="PICTURE" Style="{StaticResource Label}"/>
          <CheckBox x:Name="displaySoft" Style="{StaticResource Switch}" Content="Soft display: blends each line with its neighbours, like the Xbox's video output (off is sharper)"/>
          <CheckBox x:Name="displayVsync" Style="{StaticResource Switch}" Content="VSync: shows each frame on the display's refresh, so the picture never tears"/>
        </StackPanel>

        <!-- AUDIO -->
        <StackPanel x:Name="pageAudio" Visibility="Collapsed">
          <TextBlock Text="AUDIO" Style="{StaticResource Heading}"/>
          <TextBlock Text="Music, voices and sound effects, mixed by the port at 48 kHz stereo." Style="{StaticResource Sub}"/>
          <TextBlock Text="OUTPUT" Style="{StaticResource Label}"/>
          <CheckBox x:Name="audioSound" Style="{StaticResource Switch}" Content="Sound on"/>
          <CheckBox x:Name="audioRecord" Style="{StaticResource Switch}" Content="Record everything the game plays to logs\sound.wav"/>
          <WrapPanel Margin="0,8,0,0"><Button x:Name="openRecording" Style="{StaticResource Btn}" Content="Open the last recording"/></WrapPanel>
          <TextBlock Text="The music, effects and voice volumes are in the game's own Options menu." Style="{StaticResource Sub}" Margin="0,14,0,0"/>
        </StackPanel>

        <!-- CONTROLS -->
        <StackPanel x:Name="pageControls" Visibility="Collapsed">
          <TextBlock Text="CONTROLS" Style="{StaticResource Heading}"/>
          <TextBlock Text="Player 1 uses the keyboard and the first controller together; players 2-4 use controllers (any XInput pad)." Style="{StaticResource Sub}"/>
          <StackPanel Orientation="Horizontal">
            <StackPanel Width="450">
              <DockPanel Width="390" HorizontalAlignment="Left">
                <Button x:Name="resetKeys" DockPanel.Dock="Right" Style="{StaticResource Slot}" Content="Reset" Height="22" VerticalAlignment="Bottom"/>
                <TextBlock Text="KEYBOARD" Style="{StaticResource Label}"/>
              </DockPanel>
              <Grid x:Name="keyTable"/>
            </StackPanel>
            <StackPanel Width="370">
              <DockPanel Width="340" HorizontalAlignment="Left">
                <Button x:Name="resetPad" DockPanel.Dock="Right" Style="{StaticResource Slot}" Content="Reset" Height="22" VerticalAlignment="Bottom"/>
                <TextBlock Text="CONTROLLER" Style="{StaticResource Label}"/>
              </DockPanel>
              <Grid x:Name="padTable"/>
              <TextBlock Style="{StaticResource Sub}" TextWrapping="Wrap" Width="340" HorizontalAlignment="Left" Margin="0,12,0,0"
                         Text="Keyboard: click a key, then press the new one; right-click clears it. Controller: click to pick the next button, right-click for the previous. The d-pad and sticks stay as they are."/>
              <TextBlock Style="{StaticResource Sub}" TextWrapping="Wrap" Width="340" HorizontalAlignment="Left" Margin="0,8,0,0"
                         Text="Alt+Enter or F11 switches between the window and fullscreen."/>
            </StackPanel>
          </StackPanel>
        </StackPanel>

        <!-- SAVES -->
        <StackPanel x:Name="pageSaves" Visibility="Collapsed">
          <TextBlock Text="SAVES" Style="{StaticResource Heading}"/>
          <TextBlock Text="Story progress, unlocks and options. The game saves on its own as you play." Style="{StaticResource Sub}"/>
          <TextBlock Text="SAVES FOLDER" Style="{StaticResource Label}"/>
          <DockPanel Width="640" HorizontalAlignment="Left">
            <Button x:Name="browseSaves" DockPanel.Dock="Right" Style="{StaticResource Btn}" Content="Browse…" Margin="10,0,0,0"/>
            <TextBox x:Name="savesPath"/>
          </DockPanel>
          <WrapPanel Margin="0,14,0,0">
            <Button x:Name="openSaves" Style="{StaticResource Btn}" Content="Open the saves folder"/>
            <Button x:Name="backupSaves" Style="{StaticResource Btn}" Content="Back up saves now"/>
            <Button x:Name="openBackups" Style="{StaticResource Btn}" Content="Open backups"/>
          </WrapPanel>
          <TextBlock x:Name="backupInfo" Style="{StaticResource Sub}" Margin="0,6,0,0"/>
        </StackPanel>

        <!-- ADVANCED -->
        <StackPanel x:Name="pageAdvanced" Visibility="Collapsed">
          <TextBlock Text="ADVANCED" Style="{StaticResource Heading}"/>
          <TextBlock Text="GAME DATA FOLDER (the extracted disc)" Style="{StaticResource Label}" Margin="0,10,0,8"/>
          <DockPanel Width="640" HorizontalAlignment="Left">
            <Button x:Name="browseData" DockPanel.Dock="Right" Style="{StaticResource Btn}" Content="Browse…" Margin="10,0,0,0"/>
            <TextBox x:Name="dataPath"/>
          </DockPanel>
          <TextBlock Text="LAUNCHER" Style="{StaticResource Label}"/>
          <CheckBox x:Name="hideWhilePlaying" Style="{StaticResource Switch}" Content="Minimise the launcher while the game runs"/>
          <TextBlock Text="STARTUP" Style="{StaticResource Label}"/>
          <CheckBox x:Name="quickIntro" Style="{StaticResource Switch}" Content="Quick start: skip the studio logos and wait less at the copyright screen"/>
          <TextBlock Text="TOOLS" Style="{StaticResource Label}"/>
          <WrapPanel>
            <Button x:Name="rebuild" Style="{StaticResource Btn}" Content="Rebuild the game"/>
            <Button x:Name="extract" Style="{StaticResource Btn}" Content="Extract the disc image"/>
            <Button x:Name="openLog" Style="{StaticResource Btn}" Content="Open the last game log"/>
            <Button x:Name="openRepo" Style="{StaticResource Btn}" Content="Open the port folder"/>
          </WrapPanel>
          <TextBox x:Name="toolOutput" Height="150" Width="640" HorizontalAlignment="Left" IsReadOnly="True" FontFamily="Consolas"
                   FontSize="11" VerticalScrollBarVisibility="Auto" TextWrapping="Wrap" Margin="0,6,0,0" Visibility="Collapsed"/>
        </StackPanel>
      </Grid>

      <!-- bottom bar -->
      <Border Grid.Row="2" Grid.ColumnSpan="2" BorderBrush="#2E4C88" BorderThickness="0,1,0,0">
        <Border.Background><LinearGradientBrush StartPoint="0,0" EndPoint="0,1">
          <GradientStop Color="#C0071030" Offset="0"/><GradientStop Color="#F0030713" Offset="1"/></LinearGradientBrush></Border.Background>
        <DockPanel Margin="28,0,40,0">
          <Button x:Name="playBtn" DockPanel.Dock="Right" Style="{StaticResource Play}" VerticalAlignment="Center">
            <TextBlock x:Name="playText" Text="PLAY" FontFamily="Segoe UI Black" FontStyle="Italic" FontSize="30" Foreground="#2A0A00"/>
          </Button>
          <StackPanel VerticalAlignment="Center">
            <TextBlock x:Name="status" FontSize="15" FontFamily="Segoe UI Semibold" Foreground="#DDE8FF" TextWrapping="Wrap"/>
            <TextBlock x:Name="statusDetail" Style="{StaticResource Sub}" FontSize="12" Margin="0,3,0,0"/>
          </StackPanel>
        </DockPanel>
      </Border>
    </Grid>
  </Grid>
</Window>
'@
$window = [Windows.Markup.XamlReader]::Load((New-Object System.Xml.XmlNodeReader $xaml))
$icon = Join-Path $PSScriptRoot 'launcher\cover.ico'  # your own picture (launcher\make_icon.ps1 -Image), if made
if (-not (Test-Path $icon)) { $icon = Join-Path $PSScriptRoot 'launcher\icon.ico' }
try {
    $window.Icon = [Windows.Media.Imaging.BitmapFrame]::Create([Uri]$icon)
    # the title bar's icon: the 48-pixel picture, which scales down to 18 more cleanly than the 256 one
    $frames = [Windows.Media.Imaging.BitmapDecoder]::Create([Uri]$icon, 'None', 'OnLoad').Frames
    $window.FindName('titleIcon').Source = ($frames | Sort-Object { [math]::Abs($_.PixelWidth - 48) } | Select-Object -First 1)
} catch { }
$ui = @{}
$xaml.SelectNodes('//*[@*[local-name()="Name"]]') | ForEach-Object {
    $name = $_.Attributes | Where-Object { $_.LocalName -eq 'Name' } | Select-Object -First 1 -ExpandProperty Value
    $ui[$name] = $window.FindName($name)
}

# ------------------------------------------------------------------------------------ background animation --
$speeds = @{ ring1 = 140; ring2 = -95; ring3 = 60; ring4 = -40; ring5 = 25 }
foreach ($r in $speeds.Keys) {
    $anim = New-Object Windows.Media.Animation.DoubleAnimation
    $anim.From = 0; $anim.To = [math]::Sign($speeds[$r]) * 360
    $anim.Duration = [Windows.Duration][TimeSpan]::FromSeconds([math]::Abs($speeds[$r]))
    $anim.RepeatBehavior = [Windows.Media.Animation.RepeatBehavior]::Forever
    [Windows.Media.Animation.Timeline]::SetDesiredFrameRate($anim, 30)
    $ui[$r].RenderTransform.BeginAnimation([Windows.Media.RotateTransform]::AngleProperty, $anim)
}
# sparks: small lights drifting out of the tunnel, moved 20 times a second (paused while minimised)
$random = New-Object Random 7
$sparks = @()
for ($i = 0; $i -lt 12; $i++) {
    $size = 2 + $random.Next(5)
    $dot = New-Object Windows.Shapes.Ellipse -Property @{ Width = $size; Height = $size; Opacity = 0 }
    $dot.Fill = New-Object Windows.Media.SolidColorBrush ([Windows.Media.Color]::FromRgb(0x7A, 0xD8, 0xFF))
    $ui.sparks.Children.Add($dot) | Out-Null
    $sparks += [pscustomobject]@{ Dot = $dot; Angle = $random.NextDouble() * 2 * [math]::PI
                                  Life = 4 + $random.NextDouble() * 6; Age = $random.NextDouble() * 8 }
}
$sparkTimer = New-Object Windows.Threading.DispatcherTimer
$sparkTimer.Interval = [TimeSpan]::FromMilliseconds(50)
$sparkTimer.Add_Tick({
    if ($window.WindowState -eq 'Minimized') { return }
    foreach ($s in $sparks) {
        $s.Age += 0.05
        if ($s.Age -ge $s.Life) { $s.Age = 0; $s.Angle = $random.NextDouble() * 2 * [math]::PI }
        $k = $s.Age / $s.Life; $d = $k * $k  # speeding up as it comes towards the viewer
        [Windows.Controls.Canvas]::SetLeft($s.Dot, 740 + [math]::Cos($s.Angle) * 640 * $d)
        [Windows.Controls.Canvas]::SetTop($s.Dot, 330 + [math]::Sin($s.Angle) * 440 * $d)
        $s.Dot.Opacity = [math]::Min(1, $k * 3) * (1 - $k * 0.3)
    }
})
$sparkTimer.Start()
# ------------------------------------------------------------------------------------------------- helpers --
function Set-Status([string]$text, [string]$detail = '') { $ui.status.Text = $text; $ui.statusDetail.Text = $detail }
function Open-Path([string]$path) { if (Test-Path $path) { Start-Process explorer.exe "`"$path`"" } }

function Update-Overview {
    $xbe = Join-Path $settings.dataPath 'default.xbe'
    $ok = [Windows.Media.Brushes]::LightGreen; $bad = New-Object Windows.Media.SolidColorBrush ([Windows.Media.Color]::FromRgb(255, 120, 90))
    if (Test-Path $xbe) { $ui.dataStatus.Text = 'Ready'; $ui.dataStatus.Foreground = $ok; $ui.dataDetail.Text = Split-Path -Leaf $settings.dataPath }
    else { $ui.dataStatus.Text = 'Missing'; $ui.dataStatus.Foreground = $bad; $ui.dataDetail.Text = 'Advanced → Extract the disc image' }
    if (Test-Path $exe) {
        $ui.buildStatus.Text = 'Ready'; $ui.buildStatus.Foreground = $ok
        $ui.buildDetail.Text = 'built ' + (Get-Item (Join-Path $root 'decomp\build\dra2_native.dll') -ErrorAction SilentlyContinue).LastWriteTime.ToString('d MMM yyyy, HH:mm')
    } else { $ui.buildStatus.Text = 'Not built'; $ui.buildStatus.Foreground = $bad; $ui.buildDetail.Text = 'Advanced → Rebuild the game' }
    $u = Join-Path $settings.savesPath 'u'
    $count = @(Get-ChildItem $u -Directory -ErrorAction SilentlyContinue).Count
    if ($count) {
        $last = (Get-ChildItem $u -Recurse -File | Sort-Object LastWriteTime | Select-Object -Last 1).LastWriteTime
        $ui.saveStatus.Text = "$count save" + $(if ($count -gt 1) { 's' } else { '' }); $ui.saveStatus.Foreground = $ok
        $ui.saveDetail.Text = 'last saved ' + $last.ToString('d MMM, HH:mm')
    } else { $ui.saveStatus.Text = 'None yet'; $ui.saveStatus.Foreground = [Windows.Media.Brushes]::White; $ui.saveDetail.Text = 'starts a new game' }
    if (-not $script:game) {
        if ((Test-Path $xbe) -and (Test-Path $exe)) { Set-Status 'Ready to play.' 'Close the game window to come back here.' }
        else { Set-Status 'Not ready yet.' 'See the cards on the Home page.' }
    }
}

# display mode chips, on Home and on Display, kept in step
$modes = [ordered]@{ '1' = 'Window 1×'; '2' = 'Window 2×'; '3' = 'Window 3×'; '4' = 'Window 4×'; 'fullscreen' = 'Fullscreen' }
$chips = @()
foreach ($panel in 'quickDisplay', 'displayModes') {
    foreach ($key in $modes.Keys) {
        if ($panel -eq 'quickDisplay' -and $key -in '1', '4') { continue }
        $chip = New-Object Windows.Controls.RadioButton -Property @{ Content = $modes[$key]; Tag = $key; GroupName = $panel }
        $chip.Style = $window.FindResource('Chip')
        $chip.IsChecked = ($settings.display -eq $key)
        $chip.Add_Checked({
            param($s)
            $settings.display = [string]$s.Tag; Save-Settings
            foreach ($c in $chips) { if ($c.Tag -eq $s.Tag -and -not $c.IsChecked) { $c.IsChecked = $true } }
        })
        $ui[$panel].Children.Add($chip) | Out-Null
        $chips += $chip
    }
}

# picture shape: the game's 4:3, or wider (DRA2_ASPECT): more of the scene at the sides, nothing stretched
$aspects = [ordered]@{ '4:3' = '4:3  (original)'; '16:10' = '16:10'; '16:9' = '16:9'; '21:9' = '21:9' }
foreach ($key in $aspects.Keys) {
    $chip = New-Object Windows.Controls.RadioButton -Property @{ Content = $aspects[$key]; Tag = $key; GroupName = 'aspect' }
    $chip.Style = $window.FindResource('Chip')
    $chip.IsChecked = ($settings.aspect -eq $key)
    $chip.Add_Checked({ param($s) $settings.aspect = [string]$s.Tag; Save-Settings })
    $ui.aspectModes.Children.Add($chip) | Out-Null
}

# resolution: the lines the picture is drawn at (DRA2_RESOLUTION); the game's own is 480
$resolutions = [ordered]@{ '480' = '480p  (original)'; '720' = '720p'; '1080' = '1080p'; '1440' = '1440p'; '2160' = '4K' }
foreach ($key in $resolutions.Keys) {
    $chip = New-Object Windows.Controls.RadioButton -Property @{ Content = $resolutions[$key]; Tag = $key; GroupName = 'resolution' }
    $chip.Style = $window.FindResource('Chip')
    $chip.IsChecked = ([string]$settings.resolution -eq $key)
    $chip.Add_Checked({ param($s) $settings.resolution = [string]$s.Tag; Save-Settings })
    $ui.resolutionModes.Children.Add($chip) | Out-Null
}

# anti-aliasing: multisamples per pixel (DRA2_MSAA); the game's own is none
$msaaModes = [ordered]@{ '0' = 'Off  (original)'; '2' = '2×'; '4' = '4×'; '8' = '8×' }
foreach ($key in $msaaModes.Keys) {
    $chip = New-Object Windows.Controls.RadioButton -Property @{ Content = $msaaModes[$key]; Tag = $key; GroupName = 'msaa' }
    $chip.Style = $window.FindResource('Chip')
    $chip.IsChecked = ([string]$settings.msaa -eq $key)
    $chip.Add_Checked({ param($s) $settings.msaa = [string]$s.Tag; Save-Settings })
    $ui.msaaModes.Children.Add($chip) | Out-Null
}

# controls: rebinding the keyboard (DRA2_KEYS) and the controller's buttons (DRA2_PAD), as port/xapi.c reads them.
# The game's controls by name, with their labels and default keys (hex virtual-key codes). White and Black are the
# original Xbox controller's two extra buttons, which XInput pads don't have (LB and RB stand in for them).
$controlList = @(
    @('up', 'Up  (move, d-pad)', '26'), @('down', 'Down', '28'), @('left', 'Left', '25'), @('right', 'Right', '27'),
    @('a', 'A  (jump)', '5a'), @('b', 'B  (grab / throw)', '58'), @('x', 'X  (special attack)', '43'), @('y', 'Y', '56'),
    @('white', 'White  (extra Xbox button)', '45'), @('black', 'Black  (extra Xbox button)', '52'),
    @('lt', 'Left trigger', '51'), @('rt', 'Right trigger', '57'), @('start', 'Start  (pause)', '0d'), @('back', 'Back', '1b'),
    @('rup', 'Right stick up', '49'), @('rdown', 'Right stick down', '4b'), @('rleft', 'Right stick left', '4a'),
    @('rright', 'Right stick right', '4c'))
# the controller: which button sets each of the game's buttons
$padChoices = 'a', 'b', 'x', 'y', 'lb', 'rb', 'lt', 'rt', 'start', 'back', 'ls', 'rs'
$padLabels = @{ a = 'A'; b = 'B'; x = 'X'; y = 'Y'; lb = 'LB'; rb = 'RB'; lt = 'LT'; rt = 'RT'; start = 'Start'; back = 'Back'
                ls = 'Left stick click'; rs = 'Right stick click' }
$padList = @(
    @('a', 'A  (jump)', 'a'), @('b', 'B  (grab / throw)', 'b'), @('x', 'X  (special attack)', 'x'), @('y', 'Y', 'y'),
    @('white', 'White  (extra Xbox button)', 'lb'), @('black', 'Black  (extra Xbox button)', 'rb'), @('lt', 'Left trigger', 'lt'), @('rt', 'Right trigger', 'rt'),
    @('start', 'Start  (pause)', 'start'), @('back', 'Back', 'back'))

function Read-Bindings([string]$text) {
    $map = @{}
    foreach ($pair in ($text -split ',')) {
        $name, $value = $pair -split '=', 2
        if ($name -and $null -ne $value) { $map[$name.Trim()] = $value.Trim() }
    }
    $map
}
function Read-Keys([string]$text) {  # 'vk+vk' to two ints, 0 for none
    $vks = @(0, 0); $i = 0
    foreach ($part in ($text -split '\+')) { if ($part -and $i -lt 2) { $vks[$i++] = [Convert]::ToInt32($part, 16) } }
    , $vks
}
$keyMap = [ordered]@{}; $padMap = [ordered]@{}
function Reset-KeyMap { foreach ($c in $controlList) { $keyMap[$c[0]] = Read-Keys $c[2] } }
function Reset-PadMap { foreach ($p in $padList) { $padMap[$p[0]] = $p[2] } }
Reset-KeyMap; Reset-PadMap
try {
    $saved = Read-Bindings $settings.keys
    foreach ($n in $saved.Keys) { if ($keyMap.Contains($n)) { $keyMap[$n] = Read-Keys $saved[$n]; $keyMap[$n][1] = 0 } }  # one key each
    $saved = Read-Bindings $settings.pad
    foreach ($n in $saved.Keys) { if ($padMap.Contains($n) -and $padChoices -contains $saved[$n]) { $padMap[$n] = $saved[$n] } }
} catch { Reset-KeyMap; Reset-PadMap }

function Keys-Text {
    (@($keyMap.Keys) | ForEach-Object {
        $name = $_
        "$name=" + ((@($keyMap[$name]) | Where-Object { $_ } | ForEach-Object { '{0:x2}' -f $_ }) -join '+')
    }) -join ','
}
# the game always gets the whole map, so it uses exactly the keys shown (its own defaults add Backspace for Back)
function Save-Keys { $settings.keys = Keys-Text; Save-Settings }
function Save-Pad { $settings.pad = (@($padMap.Keys) | ForEach-Object { "$_=$($padMap[$_])" }) -join ','; Save-Settings }

function Key-Name([int]$vk) {
    if (-not $vk) { return '—' }
    $names = @{ 0x0d = 'Enter'; 0x1b = 'Esc'; 0x08 = 'Backspace'; 0x20 = 'Space'; 0x09 = 'Tab'; 0x26 = '↑'; 0x28 = '↓'
                0x25 = '←'; 0x27 = '→'; 0xa0 = 'Left Shift'; 0xa1 = 'Right Shift'; 0xa2 = 'Left Ctrl'; 0xa3 = 'Right Ctrl'
                0xa4 = 'Left Alt'; 0xa5 = 'Right Alt'; 0x2e = 'Delete'; 0x2d = 'Insert'; 0x24 = 'Home'; 0x23 = 'End'
                0x21 = 'Page Up'; 0x22 = 'Page Down' }
    if ($names.ContainsKey($vk)) { return $names[$vk] }
    $k = [Windows.Input.KeyInterop]::KeyFromVirtualKey($vk).ToString()
    if ($k -match '^D(\d)$') { return $Matches[1] }
    if ($k -match '^NumPad(\d)$') { return "Num $($Matches[1])" }
    $k -replace '^Oem', ''
}

function Add-Row($grid, [string]$text) {
    $row = $grid.RowDefinitions.Count
    $grid.RowDefinitions.Add((New-Object Windows.Controls.RowDefinition -Property @{ Height = (New-Object Windows.GridLength 23) }))
    $label = New-Object Windows.Controls.TextBlock -Property @{ Text = $text; FontSize = 13; VerticalAlignment = 'Center' }
    [Windows.Controls.Grid]::SetRow($label, $row)
    $grid.Children.Add($label) | Out-Null
    $row
}
function Add-Slot($grid, [int]$row, [int]$column, [string]$id) {
    $b = New-Object Windows.Controls.Button -Property @{ Uid = $id; Tag = '' }
    $b.Style = $window.FindResource('Slot')
    [Windows.Controls.Grid]::SetRow($b, $row); [Windows.Controls.Grid]::SetColumn($b, $column)
    $grid.Children.Add($b) | Out-Null
    $b
}

# keyboard: one key per control; click it, then press the new key (right-click clears it)
$keySlots = @()
foreach ($w in 240, 150) {
    $ui.keyTable.ColumnDefinitions.Add((New-Object Windows.Controls.ColumnDefinition -Property @{ Width = (New-Object Windows.GridLength $w) }))
}
foreach ($c in $controlList) {
    $row = Add-Row $ui.keyTable $c[1]
    for ($slot = 0; $slot -lt 1; $slot++) {
        $b = Add-Slot $ui.keyTable $row ($slot + 1) "$($c[0])|$slot"
        $b.Add_Click({ param($s) Start-Capture $s })
        $b.Add_MouseRightButtonUp({ param($s, $e) $e.Handled = $true; $script:capture = $null; Set-Key $s 0 })
        $keySlots += $b
    }
}
function Show-Keys {
    foreach ($b in $keySlots) {
        $name, $slot = $b.Uid -split '\|'
        $b.Tag = ''; $b.Content = Key-Name $keyMap[$name][[int]$slot]
    }
}
$script:capture = $null
function Start-Capture($button) {
    $was = $script:capture
    Show-Keys
    $script:capture = $null
    if ($was -eq $button) { return }  # a second click cancels
    $script:capture = $button; $button.Tag = 'waiting'; $button.Content = 'press a key…'
}
function Set-Key($button, [int]$vk) {
    $name, $slot = $button.Uid -split '\|'
    if ($vk) {  # a key does one thing: taken from wherever else it was
        foreach ($other in @($keyMap.Keys)) { for ($k = 0; $k -lt 2; $k++) { if ($keyMap[$other][$k] -eq $vk) { $keyMap[$other][$k] = 0 } } }
    }
    $keyMap[$name][[int]$slot] = $vk
    Save-Keys; Show-Keys
}
$window.Add_PreviewKeyDown({
    param($s, $e)
    if (-not $script:capture) { return }
    $e.Handled = $true
    $key = if ($e.Key -eq 'System') { $e.SystemKey } else { $e.Key }
    if ($key -eq 'F11') { return }  # the game's fullscreen key
    $vk = [Windows.Input.KeyInterop]::VirtualKeyFromKey($key)
    if (-not $vk) { return }
    $b = $script:capture; $script:capture = $null
    Set-Key $b $vk
})
$ui.resetKeys.Add_Click({ $script:capture = $null; Reset-KeyMap; $settings.keys = ''; Save-Settings; Show-Keys })
Show-Keys

# controller: click a button to pick the next choice, right-click for the previous
$padSlots = @()
foreach ($w in 190, 150) {
    $ui.padTable.ColumnDefinitions.Add((New-Object Windows.Controls.ColumnDefinition -Property @{ Width = (New-Object Windows.GridLength $w) }))
}
foreach ($p in $padList) {
    $row = Add-Row $ui.padTable $p[1]
    $b = Add-Slot $ui.padTable $row 1 $p[0]
    $b.Add_Click({ param($s) Step-Pad $s 1 })
    $b.Add_MouseRightButtonUp({ param($s, $e) $e.Handled = $true; Step-Pad $s -1 })
    $padSlots += $b
}
function Show-Pad { foreach ($b in $padSlots) { $b.Content = $padLabels[$padMap[$b.Uid]] } }
function Step-Pad($button, [int]$step) {
    $i = [array]::IndexOf($padChoices, $padMap[$button.Uid])
    $padMap[$button.Uid] = $padChoices[($i + $step + $padChoices.Count) % $padChoices.Count]
    Save-Pad; Show-Pad
}
$ui.resetPad.Add_Click({ Reset-PadMap; $settings.pad = ''; Save-Settings; Show-Pad })
Show-Pad

# --------------------------------------------------------------------------------------------- navigation --
$pages = 'Home', 'Display', 'Audio', 'Controls', 'Saves', 'Advanced'
foreach ($p in $pages) {
    $ui["nav$p"].Tag = $p
    $ui["nav$p"].Add_Checked({
        param($s)
        foreach ($q in $pages) { $ui["page$q"].Visibility = if ($q -eq $s.Tag) { 'Visible' } else { 'Collapsed' } }
        if ($s.Tag -eq 'Home') { Update-Overview }
    })
}

# ------------------------------------------------------------------------------------------------ settings --
$ui.quickIntro.IsChecked = $settings.quickIntro
$ui.quickIntro.Add_Checked({ $settings.quickIntro = $true; Save-Settings })
$ui.quickIntro.Add_Unchecked({ $settings.quickIntro = $false; Save-Settings })
$ui.audioSound.IsChecked = $settings.sound
$ui.audioRecord.IsChecked = $settings.record
$ui.hideWhilePlaying.IsChecked = $settings.hideWhilePlaying
$ui.displaySoft.IsChecked = $settings.soft
$ui.displaySoft.Add_Click({ $settings.soft = [bool]$ui.displaySoft.IsChecked; Save-Settings })
$ui.displayVsync.IsChecked = $settings.vsync
$ui.displayVsync.Add_Click({ $settings.vsync = [bool]$ui.displayVsync.IsChecked; Save-Settings })
$ui.savesPath.Text = $settings.savesPath
$ui.dataPath.Text = $settings.dataPath
$ui.audioSound.Add_Click({ $settings.sound = [bool]$ui.audioSound.IsChecked; Save-Settings })
$ui.audioRecord.Add_Click({ $settings.record = [bool]$ui.audioRecord.IsChecked; Save-Settings })
$ui.hideWhilePlaying.Add_Click({ $settings.hideWhilePlaying = [bool]$ui.hideWhilePlaying.IsChecked; Save-Settings })
$ui.savesPath.Add_LostFocus({ $settings.savesPath = $ui.savesPath.Text.Trim(); Save-Settings })
$ui.dataPath.Add_LostFocus({ $settings.dataPath = $ui.dataPath.Text.Trim(); Save-Settings; Update-Overview })

function Select-Folder([string]$start) {
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    if (Test-Path $start) { $dialog.SelectedPath = $start }
    if ($dialog.ShowDialog() -eq 'OK') { return $dialog.SelectedPath }
    return $null
}
$ui.browseSaves.Add_Click({
    $f = Select-Folder $settings.savesPath
    if ($f) { $settings.savesPath = $f; $ui.savesPath.Text = $f; Save-Settings; Update-Overview }
})
$ui.browseData.Add_Click({
    $f = Select-Folder $settings.dataPath
    if ($f) { $settings.dataPath = $f; $ui.dataPath.Text = $f; Save-Settings; Update-Overview }
})
$ui.openSaves.Add_Click({ New-Item -ItemType Directory -Force $settings.savesPath | Out-Null; Open-Path $settings.savesPath })
$ui.openBackups.Add_Click({ New-Item -ItemType Directory -Force $backups | Out-Null; Open-Path $backups })
$ui.backupSaves.Add_Click({
    $u = Join-Path $settings.savesPath 'u'
    if (-not (Test-Path $u)) { $ui.backupInfo.Text = 'There are no saves to back up yet.'; return }
    $to = Join-Path $backups (Get-Date -Format 'yyyy-MM-dd_HH-mm-ss')
    New-Item -ItemType Directory -Force $to | Out-Null
    Copy-Item $u $to -Recurse
    $ui.backupInfo.Text = "Backed up to $to. To restore, copy its u folder back into the saves folder."
})
$ui.openRecording.Add_Click({ $w = Join-Path $logs 'sound.wav'; if (Test-Path $w) { Start-Process $w } })
$ui.openLog.Add_Click({ $l = Join-Path $logs 'last_run.txt'; if (Test-Path $l) { Start-Process notepad.exe "`"$l`"" } })
$ui.openRepo.Add_Click({ Open-Path $root })

# -------------------------------------------------------------------------------------- background tools --
$script:tool = $null
function Start-Tool([string]$title, [string[]]$arguments) {
    if ($script:tool -or $script:game) { return }
    New-Item -ItemType Directory -Force $logs | Out-Null
    $script:toolLog = Join-Path $logs 'tool.txt'
    Set-Content $script:toolLog "$title...`r`n"
    $ui.toolOutput.Visibility = 'Visible'
    $script:tool = Start-Process powershell -ArgumentList $arguments -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $script:toolLog -RedirectStandardError (Join-Path $logs 'tool_errors.txt')
    $null = $script:tool.Handle
    $ui.rebuild.IsEnabled = $false; $ui.extract.IsEnabled = $false; $ui.playBtn.IsEnabled = $false
    Set-Status "$title..." 'This can take a few minutes.'
    $script:toolTitle = $title
}
$ui.rebuild.Add_Click({
    $env:DRA2_DATA = $settings.dataPath  # the build reads default.xbe from there
    Start-Tool 'Building the game' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$(Join-Path $root 'tools\build_native.ps1')`"")
})
$ui.extract.Add_Click({
    $iso = Get-ChildItem $root -Filter *.iso | Select-Object -First 1
    if (-not $iso) {
        $dialog = New-Object Microsoft.Win32.OpenFileDialog -Property @{ Filter = 'Disc image (*.iso)|*.iso'; Title = 'Choose your disc image' }
        if (-not $dialog.ShowDialog()) { return }
        $iso = Get-Item $dialog.FileName
    }
    $command = "python -I `"$(Join-Path $root 'tools\xdvdfs_extract.py')`" `"$($iso.FullName)`" `"$($settings.dataPath)`""
    Start-Tool 'Extracting the disc image' @('-NoProfile', '-Command', $command)
})

# ------------------------------------------------------------------------------------------------- playing --
$script:game = $null
function Start-Game {
    if ($script:game -or $script:tool) { return }
    Update-Overview
    if (-not (Test-Path (Join-Path $settings.dataPath 'default.xbe'))) { Set-Status 'The game data is missing.' 'Advanced → Extract the disc image, or point the game data folder at it.'; return }
    if (-not (Test-Path $exe)) { Set-Status 'The game isn''t built yet.' 'Advanced → Rebuild the game.'; return }
    New-Item -ItemType Directory -Force $settings.savesPath, $logs | Out-Null
    foreach ($name in 'DRA2_INPUT', 'DRA2_ARENA', 'DRA2_SNAPSHOT', 'DRA2_LOGFRAME', 'DRA2_LOGDRAW', 'DRA2_SAMPLE',
                      'DRA2_MUTE', 'DRA2_SOUNDDUMP', 'DRA2_LOGSOUND', 'DRA2_WINDOW', 'DRA2_SOFT', 'DRA2_ASPECT',
                      'DRA2_RESOLUTION', 'DRA2_KEYS', 'DRA2_PAD', 'DRA2_QUICKINTRO', 'DRA2_MSAA', 'DRA2_VSYNC') {
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    $env:DRA2_DATA = $settings.dataPath
    $env:DRA2_SAVES = $settings.savesPath
    $env:DRA2_WINDOW = $settings.display
    if (-not $settings.soft) { $env:DRA2_SOFT = '0' }
    if ($settings.aspect -ne '4:3') { $env:DRA2_ASPECT = $settings.aspect }
    $env:DRA2_RESOLUTION = [string]$settings.resolution
    $env:DRA2_MSAA = [string]$settings.msaa
    if ($settings.vsync) { $env:DRA2_VSYNC = '1' }
    $env:DRA2_KEYS = Keys-Text
    if ($settings.pad) { $env:DRA2_PAD = [string]$settings.pad }
    if (-not $settings.sound) { $env:DRA2_MUTE = '1' }
    if ($settings.quickIntro) { $env:DRA2_QUICKINTRO = '1' }
    if ($settings.record) { $env:DRA2_SOUNDDUMP = Join-Path $logs 'sound.wav' }
    $script:gameLog = Join-Path $logs 'last_run.txt'
    # -NoNewWindow: the game is a console program, and would otherwise get a console window of its own. It shares the
    # launcher's hidden one instead; its output goes to the log either way.
    $script:game = Start-Process $exe -PassThru -NoNewWindow -WorkingDirectory $logs -RedirectStandardOutput $script:gameLog `
        -RedirectStandardError (Join-Path $logs 'last_run_errors.txt')
    $null = $script:game.Handle
    $script:started = Get-Date
    $ui.playText.Text = 'RUNNING'; $ui.playBtn.IsEnabled = $false
    Set-Status 'The game is running.' 'Close its window to come back here.'
    if ($settings.hideWhilePlaying) { $window.WindowState = 'Minimized' }
}
$ui.playBtn.Add_Click({ Start-Game })

$timer = New-Object Windows.Threading.DispatcherTimer
$timer.Interval = [TimeSpan]::FromMilliseconds(500)
$timer.Add_Tick({
    if ($script:game -and $script:game.HasExited) {
        $minutes = [math]::Round(((Get-Date) - $script:started).TotalMinutes, 1)
        $problems = @(Select-String $script:gameLog -Pattern '^\[crash\]|Fatal|fatal error' -ErrorAction SilentlyContinue | Select-Object -First 3)
        $script:game = $null
        $ui.playText.Text = 'PLAY'; $ui.playBtn.IsEnabled = $true
        $window.WindowState = 'Normal'; $window.Activate() | Out-Null
        Update-Overview
        if ($problems) { Set-Status "The game stopped with an error after $minutes min." (($problems | ForEach-Object Line) -join '  |  ') }
        else { Set-Status "Played for $minutes min." 'Ready to play again.' }
    }
    if ($script:tool) {
        $text = Get-Content $script:toolLog -Raw -ErrorAction SilentlyContinue
        if ($text -ne $ui.toolOutput.Text) { $ui.toolOutput.Text = $text; $ui.toolOutput.ScrollToEnd() }
        if ($script:tool.HasExited) {
            $code = $script:tool.ExitCode
            $script:tool = $null
            $ui.rebuild.IsEnabled = $true; $ui.extract.IsEnabled = $true; $ui.playBtn.IsEnabled = $true
            Update-Overview
            if ($code) { Set-Status "$script:toolTitle failed." 'See the output on the Advanced page.' }
            else { Set-Status "$($script:toolTitle): done." '' }
        }
    }
})
$timer.Start()

# ------------------------------------------------------------------------------------------------- window --
$ui.titleBar.Add_MouseLeftButtonDown({ $window.DragMove() })
$ui.closeBtn.Add_Click({ $window.Close() })
$ui.minBtn.Add_Click({ $window.WindowState = 'Minimized' })
$window.Add_KeyDown({ param($s, $e) if ($e.Key -eq 'Return' -and -not $script:game -and $e.OriginalSource -isnot [Windows.Controls.TextBox]) { Start-Game } })
Update-Overview
$window.ShowDialog() | Out-Null
