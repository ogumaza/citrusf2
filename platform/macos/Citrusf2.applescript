-- SPDX-License-Identifier: MIT

-- Drop 3DS sound archives (.bcsar) here to convert them to MIDI, SoundFont and SFZ.
-- Output goes in a folder beside each archive.
-- The command-line executable is bundled in Contents/Resources.

on run
	set theFiles to choose file with prompt "Choose 3DS sound archives (.bcsar) to convert:" with multiple selections allowed
	showResults(convertFiles(theFiles, toolPath()))
end run

on open theFiles
	showResults(convertFiles(theFiles, toolPath()))
end open

on toolPath()
	return POSIX path of (path to resource "citrusf2")
end toolPath

-- Convert all inputs in one process so citrusf2 can detect output folder collisions.
-- Return {report, output folders}.
on convertFiles(theFiles, tool)
	set command to quoted form of tool
	set fileNames to {}
	repeat with f in theFiles
		set p to POSIX path of f
		set command to command & " " & quoted form of p
		set end of fileNames to lastPathComponent(p)
	end repeat

	-- Preserve output on failure by ending the shell command with echo.
	-- Its final line gives the exit status: 1 for conversion errors, >128 for signals.
	set output to do shell script command & " 2>&1; echo \"exit status $?\""
	set theLines to paragraphs of output
	set exitStatus to (word -1 of (item -1 of theLines)) as integer

	-- Reports follow input order, separated by blank lines.
	set outputs to {{}}
	repeat with i from 1 to (count of theLines) - 1
		set ln to item i of theLines
		if ln is "" then
			set end of outputs to {}
		else
			set end of item -1 of outputs to ln
		end if
	end repeat

	set report to {}
	set folders to {}
	repeat with k from 1 to count of fileNames
		set fileName to item k of fileNames
		if k > (count of outputs) then
			set end of report to fileName & ": not converted; citrusf2 stopped"
		else
			set {summary, outDir} to summarize(fileName, item k of outputs)
			if k = (count of outputs) and exitStatus > 128 then
				set summary to fileName & ": citrusf2 stopped unexpectedly (signal " & (exitStatus - 128) & ")"
			end if
			if outDir is not "" and folders does not contain outDir then set end of folders to outDir
			set end of report to summary
		end if
	end repeat
	return {joinLines(report), folders}
end convertFiles

-- Extract a one-line summary and output folder from an archive's report.
-- Return "" for the folder if none was created.
on summarize(fileName, theLines)
	set summary to ""
	set lastMessage to ""
	set outDir to ""
	set isTruncated to false
	set anyConverted to false
	repeat with ln in theLines
		set ln to contents of ln
		set lastMessage to ln
		-- The report header gives the output folder.
		if ln starts with (fileName & ": ") and ln contains " sequences -> " then
			set outDir to textAfter(ln, " sequences -> ")
		end if
		-- A truncated archive converts only what's left of it. The counts don't show that.
		if ln starts with ("warning: " & fileName & " is truncated") then set isTruncated to true
		-- The final "done:" line gives the conversion counts.
		if ln starts with "done: " then
			set summary to fileName & ": " & (text 7 thru -1 of ln)
			set anyConverted to not (ln starts with "done: 0 converted")
		end if
	end repeat

	-- If conversion failed before the summary, use the last error line.
	if summary is "" then set summary to lastMessage
	-- The note is about the sounds that did convert. It appears only when some did.
	if isTruncated and anyConverted then set summary to summary & " (the archive is truncated: some sounds are missing or silent)"
	return {summary, outDir}
end summarize

on showResults(results)
	set {reportText, folders} to results
	if (count of folders) is 0 then
		display dialog reportText buttons {"OK"} default button 1 with title "Citrusf2" with icon caution
	else
		if (count of folders) is 1 then
			set openLabel to "Open Folder"
		else
			set openLabel to "Open Folders"
		end if
		set answer to display dialog reportText buttons {"OK", openLabel} default button 2 with title "Citrusf2"
		if button returned of answer is openLabel then
			repeat with d in folders
				do shell script "open " & quoted form of (d as text)
			end repeat
		end if
	end if
end showResults

-- Return the portion of t after the first marker.
on textAfter(t, marker)
	set i to (offset of marker in t) + (length of marker)
	if i > (length of t) then return ""
	return text i thru -1 of t
end textAfter

on lastPathComponent(p)
	set saved to AppleScript's text item delimiters
	set AppleScript's text item delimiters to "/"
	set parts to text items of p
	set AppleScript's text item delimiters to saved
	return item -1 of parts
end lastPathComponent

on joinLines(theList)
	set saved to AppleScript's text item delimiters
	set AppleScript's text item delimiters to return
	set joined to theList as text
	set AppleScript's text item delimiters to saved
	return joined
end joinLines
