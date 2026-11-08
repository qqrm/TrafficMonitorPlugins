param(
    [Parameter(Mandatory = $true)][string] $Repository,
    [Parameter(Mandatory = $true)][ValidateSet('baseline', 'repaired')][string] $Scenario,
    [switch] $ExpectFailure
)

$ErrorActionPreference = 'Stop'
$repositoryPath = (Resolve-Path $Repository).Path
$testSources = $PSScriptRoot
$output = Join-Path $env:RUNNER_TEMP "hardware-monitor-validation\$Scenario"
$testBin = Join-Path $output 'bin'
New-Item -ItemType Directory -Force $output, $testBin | Out-Null

function Invoke-Checked([string] $Tool, [string[]] $Arguments) {
    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Tool exited with $LASTEXITCODE"
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.CLI.Support -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $visualStudio) { throw 'The v143 C++/CLI component is required' }
$msbuild = Join-Path $visualStudio 'MSBuild\Current\Bin\MSBuild.exe'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'

# Build the unchanged production project against the bundled real LHM assembly.
# The production DLL is never loaded by this validation job.
Invoke-Checked -Tool $msbuild -Arguments @(
    (Join-Path $repositoryPath 'TrafficMonitorPlugins.sln'), '/nologo', '/m', '/t:HardwareMonitor',
    '/p:Configuration=Release', '/p:Platform=x64', '/v:minimal',
    "/bl:$(Join-Path $output 'production.binlog')"
)
$production = Join-Path $output 'production'
New-Item -ItemType Directory -Force $production | Out-Null
Copy-Item (Join-Path $repositoryPath 'Bin\x64\Release\HardwareMonitor.dll') $production

$pluginDirectory = Join-Path $repositoryPath 'Plugins\HardwareMonitor'
$vendor = Join-Path $pluginDirectory 'LibreHardwareMonitorLib.dll'
$fakeAssembly = Join-Path $testBin 'HardwareMonitorTestHardware.dll'
Invoke-Checked -Tool $compiler -Arguments @(
    '/nologo', '/target:library', '/platform:x64', "/out:$fakeAssembly", "/reference:$vendor",
    '/reference:System.Windows.Forms.dll', '/reference:System.Drawing.dll',
    (Join-Path $testSources 'TestHardware.cs')
)

# Generate a separate project; do not edit any production source or project.
# It compiles the same plugin sources and resources, with only the acquisition
# provider and modal diagnostic sink replaced for deterministic fault injection.
$projectFile = Join-Path $pluginDirectory 'HardwareMonitor.vcxproj'
$generatedProject = Join-Path $pluginDirectory 'HardwareMonitor.RecoveryValidation.vcxproj'
if (Test-Path $generatedProject) { throw "Refusing to overwrite $generatedProject" }
$project = New-Object System.Xml.XmlDocument
$project.Load($projectFile)
$namespace = $project.DocumentElement.NamespaceURI
$namespaces = [System.Xml.XmlNamespaceManager]::new($project.NameTable)
$namespaces.AddNamespace('m', $namespace)
$targets = $project.SelectSingleNode('/m:Project/m:Import[contains(@Project, "Microsoft.Cpp.targets")]', $namespaces)
if (-not $targets) { throw 'C++ targets import not found' }

function New-Element([string] $Name, [string] $Value) {
    $element = $project.CreateElement($Name, $namespace)
    if ($Value) { $element.InnerText = $Value }
    return ,$element
}

$properties = New-Element 'PropertyGroup' ''
[void] $properties.AppendChild((New-Element 'OutDir' ($testBin + '\')))
[void] $properties.AppendChild((New-Element 'IntDir' ((Join-Path $output 'obj') + '\')))
[void] $properties.AppendChild((New-Element 'TargetName' 'HardwareMonitor.RecoveryValidation'))
[void] $project.DocumentElement.InsertBefore($properties, $targets)

$definitions = New-Element 'ItemDefinitionGroup' ''
$compile = New-Element 'ClCompile' ''
[void] $compile.AppendChild((New-Element 'ForcedIncludeFiles' ((Join-Path $testSources 'Override.h') + ';%(ForcedIncludeFiles)')))
[void] $compile.AppendChild((New-Element 'AdditionalUsingDirectories' ($testBin + ';%(AdditionalUsingDirectories)')))
[void] $definitions.AppendChild($compile)
[void] $project.DocumentElement.InsertBefore($definitions, $targets)

$items = New-Element 'ItemGroup' ''
$test = New-Element 'ClCompile' ''
$test.SetAttribute('Include', (Join-Path $testSources 'RecoveryRegression.cpp'))
[void] $test.AppendChild((New-Element 'PrecompiledHeader' 'NotUsing'))
[void] $items.AppendChild($test)
$reference = New-Element 'Reference' ''
$reference.SetAttribute('Include', 'HardwareMonitorTestHardware')
[void] $reference.AppendChild((New-Element 'HintPath' $fakeAssembly))
[void] $reference.AppendChild((New-Element 'Private' 'true'))
[void] $items.AppendChild($reference)
[void] $project.DocumentElement.InsertBefore($items, $targets)

try {
    $project.Save($generatedProject)
    Invoke-Checked -Tool $msbuild -Arguments @(
        $generatedProject, '/nologo', '/m', '/p:Configuration=Release', '/p:Platform=x64',
        "/p:SolutionDir=$repositoryPath/", '/v:minimal',
        "/bl:$(Join-Path $output 'regression.binlog')"
    )
} finally {
    Remove-Item $generatedProject -ErrorAction SilentlyContinue
}

Copy-Item $vendor $testBin -Force
$runner = Join-Path $testBin 'RecoveryRunner.exe'
Invoke-Checked -Tool $compiler -Arguments @(
    '/nologo', '/target:exe', '/platform:x64', "/out:$runner", (Join-Path $testSources 'Runner.cs')
)

function Invoke-Regression([string] $Mode, [bool] $ExpectedFailure) {
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $runner
    $start.WorkingDirectory = $testBin
    $start.Arguments = $Mode
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $start
    [void] $process.Start()
    $standardOutput = $process.StandardOutput.ReadToEndAsync()
    $standardError = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(60000)) {
        $process.Kill()
        throw 'Regression timed out; a modal dialog or provider call escaped the test boundary'
    }
    $text = $standardOutput.GetAwaiter().GetResult() + $standardError.GetAwaiter().GetResult()
    $logName = if ($Mode) { 'live-startup.log' } else { 'recovery.log' }
    [IO.File]::WriteAllText((Join-Path $output $logName), $text)
    Write-Host $text
    if ($ExpectedFailure) {
        $expected = 'RECOVERY_ASSERT: startup Open failure discarded the configured sensor choices'
        if ($process.ExitCode -ne 1 -or -not $text.Contains($expected)) {
            throw 'Baseline did not fail at the expected configuration-preservation assertion'
        }
        Write-Host 'BASELINE_REPRODUCED: expected configuration-loss assertion'
    } elseif ($process.ExitCode -ne 0 -or -not $text.Contains('RECOVERY_REGRESSION_PASS')) {
        throw "Regression failed with exit $($process.ExitCode)"
    }
}

# A working live-startup control runs on both versions. The broken startup path
# must fail at the same specific assertion on the original, and pass on the fix.
Invoke-Regression -Mode '--live-startup' -ExpectedFailure $false
Invoke-Regression -Mode '' -ExpectedFailure ([bool] $ExpectFailure)
