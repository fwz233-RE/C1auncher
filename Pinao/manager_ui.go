package main

import "time"

func (m *model) handleManagement(e keyEvent, now time.Time) ([]soundCommand, string) {
	if !e.Down {
		return nil, ""
	}
	switch e.Code {
	case 103: // physical joystick up
		if m.ManagerIndex > 0 {
			m.ManagerIndex--
		}
	case 108: // physical joystick down
		if m.ManagerIndex+1 < len(m.ManagerItems) {
			m.ManagerIndex++
		}
	case 28, 352: // preview: play WAV, show archive ready to open
		if len(m.ManagerItems) > 0 {
			x := m.ManagerItems[m.ManagerIndex]
			if x.IsWAV {
				return nil, "manager-play:" + x.Path
			}
			return nil, "manager-preview:" + x.Path
		}
	case 24: // O is the explicit open button for archives
		if len(m.ManagerItems) > 0 && !m.ManagerItems[m.ManagerIndex].IsWAV {
			return nil, "manager-open:" + m.ManagerItems[m.ManagerIndex].Path
		}
	case 14, 111:
		if len(m.ManagerItems) > 0 {
			x := m.ManagerItems[m.ManagerIndex]
			if x.IsWAV {
				return nil, "manager-delete:" + x.Path
			}
		}
	case 19: // R creates a fresh archive without requiring text input.
		return nil, "manager-create"
	case 16: // Q is save-as while management screen is open.
		return nil, "manager-saveas"
	case 38, 53, 50:
		m.Management = false
	}
	return nil, ""
}
